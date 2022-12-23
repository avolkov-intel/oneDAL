/*******************************************************************************
* Copyright 2021 Intel Corporation
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

#include <type_traits>

#include "oneapi/dal/backend/primitives/reduction/reduction.hpp"
#include "oneapi/dal/backend/primitives/reduction/reduction_rm_rw_dpc.hpp"
#include "oneapi/dal/backend/primitives/reduction/reduction_rm_cw_dpc.hpp"

namespace oneapi::dal::backend::primitives {

template<typename Float, typename BinaryOp, typename UnaryOp>
Float reduce_1d_impl(sycl::queue& q,
                                const ndview<Float, 1>& input,
                                const BinaryOp& binary,
                                const UnaryOp& unary,
                                const event_vector& deps) {
    // auto [out, out_event] = ndarray<Float, 1>::full(q, std::int64_t(1), binary.init_value, sycl::usm::alloc::device);
    const auto full_deps = deps;
    Float result = Float(binary.init_value);
    sycl::buffer<Float> acc_buf{ &result, 1 };

    auto event = q.submit([&](sycl::handler& cgh) {
        cgh.depends_on(deps);
        const auto* const inp_ptr = input.get_data();
        // auto out_ptr = out.get_mutable_data();
        
        auto accum = reduction(acc_buf, cgh, binary.native);
        cgh.parallel_for(make_range_1d(input.get_count()), accum, 
            [=](sycl::id<1> idx, auto& acc) { 
                acc.combine(unary(inp_ptr[idx])); 
            }
        );
    });
    event.wait_and_throw();
    return acc_buf.get_host_access()[0];

}


template <typename Float, typename BinaryOp, typename UnaryOp>
inline sycl::event reduce_rm_rw(sycl::queue& q,
                                const ndview<Float, 2, ndorder::c>& input,
                                ndview<Float, 1>& output,
                                const BinaryOp& binary,
                                const UnaryOp& unary,
                                const event_vector& deps) {
    ONEDAL_ASSERT(input.has_data());
    ONEDAL_ASSERT(output.has_mutable_data());
    ONEDAL_ASSERT(0 <= input.get_dimension(1));
    ONEDAL_ASSERT(0 <= input.get_dimension(0));
    ONEDAL_ASSERT(input.get_dimension(0) <= output.get_dimension(0));
    using kernel_t = reduction_rm_rw<Float, BinaryOp, UnaryOp>;
    const auto width = input.get_dimension(1);
    const auto height = input.get_dimension(0);
    const auto stride = input.get_leading_stride();
    const auto* inp_ptr = input.get_data();
    auto* out_ptr = output.get_mutable_data();
    const kernel_t kernel(q);
    return kernel(inp_ptr, out_ptr, width, height, stride, binary, unary, deps);
}

template <typename Float, typename BinaryOp, typename UnaryOp>
inline sycl::event reduce_rm_cw(sycl::queue& q,
                                const ndview<Float, 2, ndorder::c>& input,
                                ndview<Float, 1>& output,
                                const BinaryOp& binary,
                                const UnaryOp& unary,
                                const event_vector& deps) {
    ONEDAL_ASSERT(input.has_data());
    ONEDAL_ASSERT(output.has_mutable_data());
    ONEDAL_ASSERT(0 <= input.get_dimension(1));
    ONEDAL_ASSERT(0 <= input.get_dimension(0));
    ONEDAL_ASSERT(input.get_dimension(1) <= output.get_dimension(0));
    using kernel_t = reduction_rm_cw<Float, BinaryOp, UnaryOp>;
    const auto width = input.get_dimension(1);
    const auto height = input.get_dimension(0);
    const auto stride = input.get_leading_stride();
    const auto* inp_ptr = input.get_data();
    auto* out_ptr = output.get_mutable_data();
    const kernel_t kernel(q);
    return kernel(inp_ptr, out_ptr, width, height, stride, binary, unary, deps);
}

template <typename Float, ndorder order, typename BinaryOp, typename UnaryOp>
sycl::event reduce_by_rows_impl(sycl::queue& q,
                                const ndview<Float, 2, order>& input,
                                ndview<Float, 1>& output,
                                const BinaryOp& binary,
                                const UnaryOp& unary,
                                const event_vector& deps) {
    ONEDAL_ASSERT(input.get_dimension(0) <= output.get_dimension(0));
    if constexpr (order == ndorder::c) {
        return reduce_rm_rw(q, input, output, binary, unary, deps);
    }
    else {
        auto input_tr = input.t();
        return reduce_rm_cw(q, input_tr, output, binary, unary, deps);
    }
    ONEDAL_ASSERT(false);
    return sycl::event{};
}

template <typename Float, ndorder order, typename BinaryOp, typename UnaryOp>
sycl::event reduce_by_columns_impl(sycl::queue& q,
                                   const ndview<Float, 2, order>& input,
                                   ndview<Float, 1>& output,
                                   const BinaryOp& binary,
                                   const UnaryOp& unary,
                                   const event_vector& deps) {
    ONEDAL_ASSERT(input.get_dimension(1) <= output.get_dimension(0));
    if constexpr (order == ndorder::c) {
        return reduce_rm_cw(q, input, output, binary, unary, deps);
    }
    else {
        auto input_tr = input.t();
        return reduce_rm_rw(q, input_tr, output, binary, unary, deps);
    }
    ONEDAL_ASSERT(false);
    return sycl::event{};
}

#define INSTANTIATE(F, L, B, U)                                                     \
    template sycl::event reduce_by_rows_impl<F, L, B, U>(sycl::queue&,              \
                                                         const ndview<F, 2, L>&,    \
                                                         ndview<F, 1>&,             \
                                                         const B&,                  \
                                                         const U&,                  \
                                                         const event_vector&);      \
    template sycl::event reduce_by_columns_impl<F, L, B, U>(sycl::queue&,           \
                                                            const ndview<F, 2, L>&, \
                                                            ndview<F, 1>&,          \
                                                            const B&,               \
                                                            const U&,               \
                                                            const event_vector&);   

#define INSTANTIATE1(F, B, U)   template F reduce_1d_impl<F, B, U>(sycl::queue&, \
                                const ndview<F, 1>&, \
                                const B&, \
                                const U&,\
                                const event_vector&);

#define INSTANTIATE_LAYOUT(F, B, U)  \
    INSTANTIATE(F, ndorder::c, B, U) \
    INSTANTIATE(F, ndorder::f, B, U)

#define INSTANTIATE_FLOAT(B, U)                       \
    INSTANTIATE_LAYOUT(double, B<double>, U<double>); \
    INSTANTIATE_LAYOUT(float, B<float>, U<float>); \
    INSTANTIATE1(float, B<float>, U<float>); \
    INSTANTIATE1(double, B<double>, U<double>);

INSTANTIATE_FLOAT(min, identity)
INSTANTIATE_FLOAT(min, abs)
INSTANTIATE_FLOAT(min, square)

INSTANTIATE_FLOAT(max, identity)
INSTANTIATE_FLOAT(max, abs)
INSTANTIATE_FLOAT(max, square)

INSTANTIATE_FLOAT(sum, identity)
INSTANTIATE_FLOAT(sum, abs)
INSTANTIATE_FLOAT(sum, square)

#undef INSTANTIATE_FLOAT

#undef INSTANTIATE_LAYOUT

#undef INSTANTIATE

} // namespace oneapi::dal::backend::primitives
