/*******************************************************************************
* Copyright 2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "oneapi/dal/algo/logistic_regression/common.hpp"
#include "oneapi/dal/algo/logistic_regression/train.hpp"
#include "oneapi/dal/backend/primitives/ndarray.hpp"
#include "oneapi/dal/backend/primitives/utils.hpp"
#include "oneapi/dal/algo/logistic_regression/infer.hpp"

#include "oneapi/dal/table/homogen.hpp"
#include "oneapi/dal/table/row_accessor.hpp"
#include "oneapi/dal/table/detail/table_builder.hpp"

#include "oneapi/dal/test/engine/fixtures.hpp"
#include "oneapi/dal/test/engine/math.hpp"

#include "oneapi/dal/test/engine/metrics/regression.hpp"
#include "oneapi/dal/backend/primitives/rng/rng_engine.hpp"

namespace oneapi::dal::logistic_regression::test {

namespace te = dal::test::engine;
namespace de = dal::detail;
namespace la = te::linalg;

namespace pr = oneapi::dal::backend::primitives;

template <typename TestType, typename Derived>
class log_reg_test : public te::crtp_algo_fixture<TestType, Derived> {
public:
    using float_t = std::tuple_element_t<0, TestType>;
    using method_t = std::tuple_element_t<1, TestType>;
    using task_t = std::tuple_element_t<2, TestType>;

    using train_input_t = train_input<task_t>;
    using train_result_t = train_result<task_t>;
    //using test_input_t = infer_input<task_t>;
    //using test_result_t = infer_result<task_t>;

    te::table_id get_homogen_table_id() const {
        return te::table_id::homogen<float_t>();
    }

    Derived* get_impl() {
        return static_cast<Derived*>(this);
    }

    auto get_descriptor() const {
        result_option_id resopts = result_options::coefficients;
        if (this->fit_intercept_)
            resopts = resopts | result_options::intercept;
        return logistic_regression::descriptor<float_t, method_t, task_t>(fit_intercept_, L2_)
            .set_result_options(resopts);
    }

    void gen_dimensions(std::int64_t n = -1, std::int64_t p = -1) {
        if (n == -1 || p == -1) {
            this->n_ = GENERATE(100, 200, 1000); //, 10000, 50000);
            this->p_ = GENERATE(10, 20); //, 30, 50);
        }
        else {
            this->n_ = n;
            this->p_ = p;
        }
    }

    float_t predict_proba(float_t* ptr, float_t* params_ptr, float_t intercept) {
        float_t val = 0;
        for (std::int64_t j = 0; j < p_; ++j) {
            val += *(ptr + j) * *(params_ptr + j);
        }
        val += intercept;
        return float_t(1) / (1 + std::exp(-val));
    }

    void gen_input(bool fit_intercept = true, double L2 = 0.0, std::int64_t seed = 2007) {
        this->get_impl()->gen_dimensions();

        this->fit_intercept_ = fit_intercept;
        this->L2_ = L2;

        std::int64_t dim = fit_intercept_ ? p_ + 1 : p_;

        X_host_ = array<float_t>::zeros(n_ * p_);
        auto* x_ptr = X_host_.get_mutable_data();

        y_host_ = array<std::int32_t>::zeros(n_);
        auto* y_ptr = y_host_.get_mutable_data();

        params_host_ = array<float_t>::zeros(dim);
        auto* params_ptr = params_host_.get_mutable_data();

        pr::rng<float_t> rn_gen;
        pr::engine eng(2007 + n_ + p_);
        rn_gen.uniform(n_ * p_, X_host_.get_mutable_data(), eng.get_state(), -10.0, 10.0);
        rn_gen.uniform(dim, params_host_.get_mutable_data(), eng.get_state(), -3.0, 3.0);

        // std::cout << "Real parameters" << std::endl;
        // for (int i = 0; i < dim; ++i) {
        //     std::cout << *(params_ptr + i) << " ";
        // }
        // std::cout << std::endl;

        for (std::int64_t i = 0; i < n_; ++i) {
            float_t val = predict_proba(x_ptr + i * p_,
                                        params_ptr + (std::int64_t)fit_intercept_,
                                        fit_intercept_ ? *params_ptr : 0);
            // float_t val = 0;
            // for (std::int64_t j = 0; j < p_; ++j) {
            //     val += *(x_ptr + i * p_ + j) * *(params_ptr + j + st_ind);
            // }
            // if (fit_intercept_) {
            //     val += *params_ptr;
            // }
            // val = float_t(1) / (1 + std::exp(-val));
            if (val < 0.5) {
                *(y_ptr + i) = 0;
            }
            else {
                *(y_ptr + i) = 1;
            }
        }
    }

    void run_test() {
        std::cout << "Test n = " << n_ << " p = " << p_ << " " << fit_intercept_ << std::endl;

        std::int64_t train_size = n_ * 0.7;
        std::int64_t test_size = n_ - train_size;

        table X_train = homogen_table::wrap<float_t>(X_host_.get_mutable_data(), train_size, p_);
        table X_test = homogen_table::wrap<float_t>(X_host_.get_mutable_data() + train_size * p_,
                                                    test_size,
                                                    p_);
        table y_train =
            homogen_table::wrap<std::int32_t>(y_host_.get_mutable_data(), train_size, 1);

        const auto desc = this->get_descriptor();
        const auto train_res = this->train(desc, X_train, y_train);
        table intercept;
        array<float_t> bias_host;
        if (fit_intercept_) {
            intercept = train_res.get_intercept();
            bias_host = row_accessor<const float_t>(intercept).pull({ 0, -1 });
            //std::cout << *(bias_host.get_mutable_data()) << " ";
        }
        table coefs = train_res.get_coefficients();
        auto coefs_host = row_accessor<const float_t>(coefs).pull({ 0, -1 });

        for (int i = 0; i < p_; ++i) {
            std::cout << *(coefs_host.get_mutable_data() + i) << " ";
        }
        std::cout << std::endl;

        std::int64_t train_acc = 0;
        std::int64_t test_acc = 0;

        for (std::int64_t i = 0; i < n_; ++i) {
            float_t val = predict_proba(X_host_.get_mutable_data() + i * p_,
                                        coefs_host.get_mutable_data(),
                                        fit_intercept_ ? *bias_host.get_mutable_data() : 0);
            std::int32_t resp = 0;
            if (val >= 0.5) {
                resp = 1;
            }
            if (resp == *(y_host_.get_mutable_data() + i)) {
                if (i < train_size) {
                    train_acc += 1;
                }
                else {
                    test_acc += 1;
                }
            }
        }

        std::cout << "Accuracy on train: " << float_t(train_acc) / train_size << " (" << train_acc
                  << " out of " << train_size << ")" << std::endl;
        std::cout << "Accuracy on test: " << float_t(test_acc) / test_size << " (" << test_acc
                  << " out of " << test_size << ")" << std::endl;

        const auto infer_res = this->infer(desc, X_test, train_res.get_model());

        table resp_table = infer_res.get_responses();
        auto resp_host = row_accessor<const float_t>(resp_table).pull({ 0, -1 });

        std::int64_t acc_algo = 0;
        for (std::int64_t i = 0; i < test_size; ++i) {
            if (*(resp_host.get_mutable_data() + i) ==
                *(y_host_.get_mutable_data() + train_size + i)) {
                acc_algo++;
            }
        }

        std::cout << "Accuracy on test(algo): " << float_t(acc_algo) / test_size << " (" << acc_algo
                  << " out of " << test_size << ")" << std::endl;
    }

protected:
    bool fit_intercept_ = true;
    double L2_ = 0.0;
    std::int64_t n_ = 0;
    std::int64_t p_ = 0;
    array<float_t> X_host_;
    array<float_t> params_host_;
    array<std::int32_t> y_host_;
    array<std::int32_t> resp_;
    // table x_test_;
};

using lr_types = COMBINE_TYPES((double),
                               (logistic_regression::method::newton_cg),
                               (logistic_regression::task::classification));

} // namespace oneapi::dal::logistic_regression::test