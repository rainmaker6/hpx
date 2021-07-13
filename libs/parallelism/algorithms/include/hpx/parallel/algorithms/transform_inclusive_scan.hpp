//  Copyright (c) 2014-2020 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

/// \file parallel/algorithms/transform_inclusive_scan.hpp

#pragma once

#include <hpx/config.hpp>
#include <hpx/concepts/concepts.hpp>
#include <hpx/functional/invoke.hpp>
#include <hpx/functional/invoke_result.hpp>
#include <hpx/functional/tag_fallback_dispatch.hpp>
#include <hpx/functional/traits/is_invocable.hpp>
#include <hpx/iterator_support/traits/is_iterator.hpp>

#include <hpx/executors/execution_policy.hpp>
#include <hpx/parallel/algorithms/detail/dispatch.hpp>
#include <hpx/parallel/algorithms/detail/distance.hpp>
#include <hpx/parallel/algorithms/inclusive_scan.hpp>
#include <hpx/parallel/util/detail/algorithm_result.hpp>
#include <hpx/parallel/util/loop.hpp>
#include <hpx/parallel/util/partitioner.hpp>
#include <hpx/parallel/util/scan_partitioner.hpp>
#include <hpx/parallel/util/zip_iterator.hpp>
#include <hpx/type_support/unused.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

namespace hpx { namespace parallel { inline namespace v1 {
    ///////////////////////////////////////////////////////////////////////////
    // transform_inclusive_scan
    namespace detail {
        /// \cond NOINTERNAL

        ///////////////////////////////////////////////////////////////////////
        // Our own version of the sequential transform_inclusive_scan.
        template <typename InIter, typename Sent, typename OutIter,
            typename Conv, typename T, typename Op>
        OutIter sequential_transform_inclusive_scan(
            InIter first, Sent last, OutIter dest, Conv&& conv, T init, Op&& op)
        {
            for (/**/; first != last; (void) ++first, ++dest)
            {
                init = HPX_INVOKE(op, init, HPX_INVOKE(conv, *first));
                *dest = init;
            }
            return dest;
        }

        template <typename InIter, typename Sent, typename OutIter,
            typename Conv, typename Op>
        OutIter sequential_transform_inclusive_scan_noinit(
            InIter first, Sent last, OutIter dest, Conv&& conv, Op&& op)
        {
            if (first != last)
            {
                auto init = HPX_INVOKE(conv, *first);

                *dest++ = init;
                return sequential_transform_inclusive_scan(++first, last, dest,
                    std::forward<Conv>(conv), std::move(init),
                    std::forward<Op>(op));
            }
            return dest;
        }

        template <typename InIter, typename OutIter, typename Conv, typename T,
            typename Op>
        T sequential_transform_inclusive_scan_n(InIter first, std::size_t count,
            OutIter dest, Conv&& conv, T init, Op&& op)
        {
            for (/**/; count-- != 0; (void) ++first, ++dest)
            {
                init = HPX_INVOKE(op, init, HPX_INVOKE(conv, *first));
                *dest = init;
            }
            return init;
        }

        ///////////////////////////////////////////////////////////////////////
        template <typename FwdIter2>
        struct transform_inclusive_scan
          : public detail::algorithm<transform_inclusive_scan<FwdIter2>,
                FwdIter2>
        {
            transform_inclusive_scan()
              : transform_inclusive_scan::algorithm("transform_inclusive_scan")
            {
            }

            template <typename ExPolicy, typename InIter, typename Sent,
                typename OutIter, typename Conv, typename T, typename Op>
            static OutIter sequential(ExPolicy, InIter first, Sent last,
                OutIter dest, Conv&& conv, T&& init, Op&& op)
            {
                return sequential_transform_inclusive_scan(first, last, dest,
                    std::forward<Conv>(conv), std::forward<T>(init),
                    std::forward<Op>(op));
            }

            template <typename ExPolicy, typename InIter, typename Sent,
                typename OutIter, typename Conv, typename Op>
            static OutIter sequential(ExPolicy&&, InIter first, Sent last,
                OutIter dest, Conv&& conv, Op&& op)
            {
                return sequential_transform_inclusive_scan_noinit(first, last,
                    dest, std::forward<Conv>(conv), std::forward<Op>(op));
            }

            template <typename ExPolicy, typename FwdIter1, typename Sent,
                typename Conv, typename T, typename Op>
            static typename util::detail::algorithm_result<ExPolicy,
                FwdIter2>::type
            parallel(ExPolicy&& policy, FwdIter1 first, Sent last,
                FwdIter2 dest, Conv&& conv, T init, Op&& op)
            {
                using result =
                    util::detail::algorithm_result<ExPolicy, FwdIter2>;
                using zip_iterator =
                    hpx::util::zip_iterator<FwdIter1, FwdIter2>;
                using difference_type =
                    typename std::iterator_traits<FwdIter1>::difference_type;

                if (first == last)
                    return result::get(std::move(dest));

                difference_type count = detail::distance(first, last);

                FwdIter2 final_dest = dest;
                std::advance(final_dest, count);

                // The overall scan algorithm is performed by executing 2
                // subsequent parallel steps. The first calculates the scan
                // results for each partition and the second produces the
                // overall result

                using hpx::get;
                using hpx::util::make_zip_iterator;

                auto f3 = [op](zip_iterator part_begin, std::size_t part_size,
                              hpx::shared_future<T> curr,
                              hpx::shared_future<T> next) -> void {
                    next.get();    // rethrow exceptions

                    T val = curr.get();
                    FwdIter2 dst = get<1>(part_begin.get_iterator_tuple());

                    util::loop_n<std::decay_t<ExPolicy>>(
                        dst, part_size, [&op, &val](FwdIter2 it) -> void {
                            *it = HPX_INVOKE(op, val, *it);
                        });
                };

                return util::scan_partitioner<ExPolicy, FwdIter2, T>::call(
                    std::forward<ExPolicy>(policy),
                    make_zip_iterator(first, dest), count, init,
                    // step 1 performs first part of scan algorithm
                    [op, conv](
                        zip_iterator part_begin, std::size_t part_size) -> T {
                        T part_init = HPX_INVOKE(conv, get<0>(*part_begin));
                        get<1>(*part_begin++) = part_init;

                        auto iters = part_begin.get_iterator_tuple();
                        return sequential_transform_inclusive_scan_n(
                            get<0>(iters), part_size - 1, get<1>(iters), conv,
                            part_init, op);
                    },
                    // step 2 propagates the partition results from left
                    // to right
                    hpx::unwrapping(op),
                    // step 3 runs final accumulation on each partition
                    std::move(f3),
                    // step 4 use this return value
                    [final_dest](std::vector<hpx::shared_future<T>>&&,
                        std::vector<hpx::future<void>>&&) -> FwdIter2 {
                        return final_dest;
                    });
            }

            template <typename ExPolicy, typename FwdIter1, typename Sent,
                typename Conv, typename Op>
            static typename util::detail::algorithm_result<ExPolicy,
                FwdIter2>::type
            parallel(ExPolicy&& policy, FwdIter1 first, Sent last,
                FwdIter2 dest, Conv&& conv, Op&& op)
            {
                if (first != last)
                {
                    auto init = HPX_INVOKE(conv, *first);

                    *dest++ = init;
                    return parallel(std::forward<ExPolicy>(policy), ++first,
                        last, dest, std::forward<Conv>(conv), std::move(init),
                        std::forward<Op>(op));
                }

                using result =
                    util::detail::algorithm_result<ExPolicy, FwdIter2>;
                return result::get(std::move(dest));
            }
        };
        /// \endcond
    }    // namespace detail

    ///////////////////////////////////////////////////////////////////////////
    /// Assigns through each iterator \a i in [result, result + (last - first))
    /// the value of
    /// GENERALIZED_NONCOMMUTATIVE_SUM(op, init, conv(*first), ...,
    /// conv(*(first + (i - result)))).
    ///
    /// \note   Complexity: O(\a last - \a first) applications of the
    ///         predicate \a op.
    ///
    /// \tparam ExPolicy    The type of the execution policy to use (deduced).
    ///                     It describes the manner in which the execution
    ///                     of the algorithm may be parallelized and the manner
    ///                     in which it executes the assignments.
    /// \tparam FwdIter1    The type of the source iterators used (deduced).
    ///                     This iterator type must meet the requirements of an
    ///                     forward iterator.
    /// \tparam FwdIter2    The type of the iterator representing the
    ///                     destination range (deduced).
    ///                     This iterator type must meet the requirements of an
    ///                     forward iterator.
    /// \tparam Conv        The type of the unary function object used for
    ///                     the conversion operation.
    /// \tparam T           The type of the value to be used as initial (and
    ///                     intermediate) values (deduced).
    /// \tparam Op          The type of the binary function object used for
    ///                     the reduction operation.
    ///
    /// \param policy       The execution policy to use for the scheduling of
    ///                     the iterations.
    /// \param first        Refers to the beginning of the sequence of elements
    ///                     the algorithm will be applied to.
    /// \param last         Refers to the end of the sequence of elements the
    ///                     algorithm will be applied to.
    /// \param dest         Refers to the beginning of the destination range.
    /// \param conv         Specifies the function (or function object) which
    ///                     will be invoked for each of the elements in the
    ///                     sequence specified by [first, last). This is a
    ///                     unary predicate. The signature of this predicate
    ///                     should be equivalent to:
    ///                     \code
    ///                     R fun(const Type &a);
    ///                     \endcode \n
    ///                     The signature does not need to have const&, but
    ///                     the function must not modify the objects passed to
    ///                     it. The type \a Type must be such that an object of
    ///                     type \a FwdIter1 can be dereferenced and then
    ///                     implicitly converted to Type.
    ///                     The type \a R must be such that an object of this
    ///                     type can be implicitly converted to \a T.
    /// \param init         The initial value for the generalized sum.
    /// \param op           Specifies the function (or function object) which
    ///                     will be invoked for each of the values of the input
    ///                     sequence. This is a
    ///                     binary predicate. The signature of this predicate
    ///                     should be equivalent to:
    ///                     \code
    ///                     Ret fun(const Type1 &a, const Type1 &b);
    ///                     \endcode \n
    ///                     The signature does not need to have const&, but
    ///                     the function must not modify the objects passed to
    ///                     it.
    ///                     The types \a Type1 and \a Ret must be
    ///                     such that an object of a type as given by the input
    ///                     sequence can be implicitly converted to any
    ///                     of those types.
    ///
    /// The reduce operations in the parallel \a transform_inclusive_scan algorithm
    /// invoked
    /// with an execution policy object of type \a sequenced_policy
    /// execute in sequential order in the calling thread.
    ///
    /// The reduce operations in the parallel \a transform_inclusive_scan algorithm
    /// invoked
    /// with an execution policy object of type \a parallel_policy
    /// or \a parallel_task_policy are permitted to execute in an unordered
    /// fashion in unspecified threads, and indeterminately sequenced
    /// within each thread.
    ///
    /// \returns  The \a transform_inclusive_scan algorithm returns a
    ///           \a hpx::future<FwdIter2> if
    ///           the execution policy is of type
    ///           \a sequenced_task_policy or
    ///           \a parallel_task_policy and
    ///           returns \a FwdIter2 otherwise.
    ///           The \a transform_inclusive_scan algorithm returns the output iterator
    ///           to the element in the destination range, one past the last
    ///           element copied.
    ///
    /// \note   GENERALIZED_NONCOMMUTATIVE_SUM(op, a1, ..., aN) is defined as:
    ///         * a1 when N is 1
    ///         * op(GENERALIZED_NONCOMMUTATIVE_SUM(op, a1, ..., aK),
    ///           GENERALIZED_NONCOMMUTATIVE_SUM(op, aM, ..., aN))
    ///           where 1 < K+1 = M <= N.
    ///
    /// Neither \a conv nor \a op shall invalidate iterators or subranges, or
    /// modify elements in the ranges [first,last) or [result,result + (last - first)).
    ///
    /// The difference between \a exclusive_scan and \a transform_inclusive_scan is that
    /// \a transform_inclusive_scan includes the ith input element in the ith sum. If
    /// \a op is not mathematically associative, the behavior of
    /// \a transform_inclusive_scan may be non-deterministic.
    ///
    // clang-format off
    template <typename ExPolicy, typename FwdIter1, typename FwdIter2,
        typename Op, typename Conv, typename T,
        HPX_CONCEPT_REQUIRES_(
            hpx::is_execution_policy<ExPolicy>::value &&
            hpx::traits::is_iterator<FwdIter1>::value &&
            hpx::traits::is_iterator<FwdIter2>::value &&
            hpx::is_invocable_v<Conv,
                typename std::iterator_traits<FwdIter1>::value_type> &&
            hpx::is_invocable_v<Op,
                typename hpx::util::invoke_result<Conv,
                    typename std::iterator_traits<FwdIter1>::value_type>::type,
                typename hpx::util::invoke_result<Conv,
                    typename std::iterator_traits<FwdIter1>::value_type>::type
            >
        )>
    // clang-format on
    HPX_DEPRECATED_V(1, 8,
        "hpx::parallel::transform_inclusive_scan is deprecated, use "
        "hpx::transform_inclusive_scan instead")
        typename util::detail::algorithm_result<ExPolicy, FwdIter2>::type
        transform_inclusive_scan(ExPolicy&& policy, FwdIter1 first,
            FwdIter1 last, FwdIter2 dest, Op&& op, Conv&& conv, T init)
    {
        static_assert(hpx::traits::is_forward_iterator<FwdIter1>::value,
            "Requires at least forward iterator.");
        static_assert(hpx::traits::is_forward_iterator<FwdIter2>::value,
            "Requires at least forward iterator.");

#if defined(HPX_GCC_VERSION) && HPX_GCC_VERSION >= 100000
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        return hpx::parallel::v1::detail::transform_inclusive_scan<FwdIter2>()
            .call(std::forward<ExPolicy>(policy), first, last, dest,
                std::forward<Conv>(conv), std::move(init),
                std::forward<Op>(op));
#if defined(HPX_GCC_VERSION) && HPX_GCC_VERSION >= 100000
#pragma GCC diagnostic pop
#endif
    }

    ///////////////////////////////////////////////////////////////////////////
    /// Assigns through each iterator \a i in [result, result + (last - first))
    /// the value of
    /// GENERALIZED_NONCOMMUTATIVE_SUM(op, conv(*first), ...,
    /// conv(*(first + (i - result)))).
    ///
    /// \note   Complexity: O(\a last - \a first) applications of the
    ///         predicate \a op.
    ///
    /// \tparam ExPolicy    The type of the execution policy to use (deduced).
    ///                     It describes the manner in which the execution
    ///                     of the algorithm may be parallelized and the manner
    ///                     in which it executes the assignments.
    /// \tparam FwdIter1    The type of the source iterators used (deduced).
    ///                     This iterator type must meet the requirements of an
    ///                     forward iterator.
    /// \tparam FwdIter2    The type of the iterator representing the
    ///                     destination range (deduced).
    ///                     This iterator type must meet the requirements of an
    ///                     forward iterator.
    /// \tparam Conv        The type of the unary function object used for
    ///                     the conversion operation.
    /// \tparam T           The type of the value to be used as initial (and
    ///                     intermediate) values (deduced).
    /// \tparam Op          The type of the binary function object used for
    ///                     the reduction operation.
    ///
    /// \param policy       The execution policy to use for the scheduling of
    ///                     the iterations.
    /// \param first        Refers to the beginning of the sequence of elements
    ///                     the algorithm will be applied to.
    /// \param last         Refers to the end of the sequence of elements the
    ///                     algorithm will be applied to.
    /// \param dest         Refers to the beginning of the destination range.
    /// \param conv         Specifies the function (or function object) which
    ///                     will be invoked for each of the elements in the
    ///                     sequence specified by [first, last). This is a
    ///                     unary predicate. The signature of this predicate
    ///                     should be equivalent to:
    ///                     \code
    ///                     R fun(const Type &a);
    ///                     \endcode \n
    ///                     The signature does not need to have const&, but
    ///                     the function must not modify the objects passed to
    ///                     it. The type \a Type must be such that an object of
    ///                     type \a FwdIter1 can be dereferenced and then
    ///                     implicitly converted to Type.
    ///                     The type \a R must be such that an object of this
    ///                     type can be implicitly converted to \a T.
    /// \param op           Specifies the function (or function object) which
    ///                     will be invoked for each of the values of the input
    ///                     sequence. This is a
    ///                     binary predicate. The signature of this predicate
    ///                     should be equivalent to:
    ///                     \code
    ///                     Ret fun(const Type1 &a, const Type1 &b);
    ///                     \endcode \n
    ///                     The signature does not need to have const&, but
    ///                     the function must not modify the objects passed to
    ///                     it.
    ///                     The types \a Type1 and \a Ret must be
    ///                     such that an object of a type as given by the input
    ///                     sequence can be implicitly converted to any
    ///                     of those types.
    ///
    /// The reduce operations in the parallel \a transform_inclusive_scan algorithm
    /// invoked
    /// with an execution policy object of type \a sequenced_policy
    /// execute in sequential order in the calling thread.
    ///
    /// The reduce operations in the parallel \a transform_inclusive_scan algorithm
    /// invoked
    /// with an execution policy object of type \a parallel_policy
    /// or \a parallel_task_policy are permitted to execute in an unordered
    /// fashion in unspecified threads, and indeterminately sequenced
    /// within each thread.
    ///
    /// \returns  The \a transform_inclusive_scan algorithm returns a
    ///           \a hpx::future<FwdIter2> if
    ///           the execution policy is of type
    ///           \a sequenced_task_policy or
    ///           \a parallel_task_policy and
    ///           returns \a FwdIter2 otherwise.
    ///           The \a transform_inclusive_scan algorithm returns the output iterator
    ///           to the element in the destination range, one past the last
    ///           element copied.
    ///
    /// \note   GENERALIZED_NONCOMMUTATIVE_SUM(op, a1, ..., aN) is defined as:
    ///         * a1 when N is 1
    ///         * op(GENERALIZED_NONCOMMUTATIVE_SUM(op, a1, ..., aK),
    ///           GENERALIZED_NONCOMMUTATIVE_SUM(op, aM, ..., aN))
    ///           where 1 < K+1 = M <= N.
    ///
    /// Neither \a conv nor \a op shall invalidate iterators or subranges, or
    /// modify elements in the ranges [first,last) or [result,result + (last - first)).
    ///
    /// The difference between \a exclusive_scan and \a transform_inclusive_scan is that
    /// \a transform_inclusive_scan includes the ith input element in the ith sum.
    ///
    // clang-format off
    template <typename ExPolicy, typename FwdIter1, typename FwdIter2,
        typename Conv, typename Op,
        HPX_CONCEPT_REQUIRES_(
            hpx::is_execution_policy<ExPolicy>::value &&
            hpx::traits::is_iterator<FwdIter1>::value &&
            hpx::traits::is_iterator<FwdIter2>::value &&
            hpx::is_invocable_v<Conv,
                typename std::iterator_traits<FwdIter1>::value_type> &&
            hpx::is_invocable_v<Op,
                typename hpx::util::invoke_result<Conv,
                    typename std::iterator_traits<FwdIter1>::value_type>::type,
                typename hpx::util::invoke_result<Conv,
                    typename std::iterator_traits<FwdIter1>::value_type>::type
            >
        )>
    // clang-format on
    HPX_DEPRECATED_V(1, 8,
        "hpx::parallel::transform_inclusive_scan is deprecated, use "
        "hpx::transform_inclusive_scan instead")
        typename util::detail::algorithm_result<ExPolicy, FwdIter2>::type
        transform_inclusive_scan(ExPolicy&& policy, FwdIter1 first,
            FwdIter1 last, FwdIter2 dest, Op&& op, Conv&& conv)
    {
        static_assert(hpx::traits::is_forward_iterator<FwdIter1>::value,
            "Requires at least forward iterator.");
        static_assert(hpx::traits::is_forward_iterator<FwdIter2>::value,
            "Requires at least forward iterator.");

#if defined(HPX_GCC_VERSION) && HPX_GCC_VERSION >= 100000
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        return hpx::parallel::v1::detail::transform_inclusive_scan<FwdIter2>()
            .call(std::forward<ExPolicy>(policy), first, last, dest,
                std::forward<Conv>(conv), std::forward<Op>(op));
#if defined(HPX_GCC_VERSION) && HPX_GCC_VERSION >= 100000
#pragma GCC diagnostic pop
#endif
    }
}}}    // namespace hpx::parallel::v1

namespace hpx {
    ///////////////////////////////////////////////////////////////////////////
    // DPO for hpx::transform_inclusive_scan
    HPX_INLINE_CONSTEXPR_VARIABLE struct transform_inclusive_scan_t final
      : hpx::functional::tag_fallback<transform_inclusive_scan_t>
    {
        // clang-format off
        template <typename InIter, typename OutIter, typename BinOp,
            typename UnOp,
            HPX_CONCEPT_REQUIRES_(
                hpx::traits::is_iterator<InIter>::value &&
                hpx::traits::is_iterator<OutIter>::value &&
                hpx::is_invocable_v<UnOp,
                    typename std::iterator_traits<InIter>::value_type> &&
                hpx::is_invocable_v<BinOp,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<InIter>::value_type>::type,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<InIter>::value_type>::type
                >
            )>
        // clang-format on
        friend OutIter tag_fallback_dispatch(hpx::transform_inclusive_scan_t,
            InIter first, InIter last, OutIter dest, BinOp&& binary_op,
            UnOp&& unary_op)
        {
            static_assert(hpx::traits::is_input_iterator<InIter>::value,
                "Requires at least input iterator.");
            static_assert(hpx::traits::is_output_iterator<OutIter>::value,
                "Requires at least output iterator.");

            return hpx::parallel::v1::detail::transform_inclusive_scan<
                OutIter>()
                .call(hpx::execution::seq, first, last, dest,
                    std::forward<UnOp>(unary_op),
                    std::forward<BinOp>(binary_op));
        }

        // clang-format off
        template <typename ExPolicy, typename FwdIter1, typename FwdIter2,
            typename BinOp, typename UnOp,
            HPX_CONCEPT_REQUIRES_(
                hpx::is_execution_policy<ExPolicy>::value &&
                hpx::traits::is_forward_iterator<FwdIter1>::value &&
                hpx::traits::is_forward_iterator<FwdIter2>::value &&
                hpx::is_invocable_v<UnOp,
                    typename std::iterator_traits<FwdIter1>::value_type> &&
                hpx::is_invocable_v<BinOp,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<FwdIter1>::value_type>::type,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<FwdIter1>::value_type>::type
                >
            )>
        // clang-format on
        friend typename parallel::util::detail::algorithm_result<ExPolicy,
            FwdIter2>::type
        tag_fallback_dispatch(hpx::transform_inclusive_scan_t,
            ExPolicy&& policy, FwdIter1 first, FwdIter1 last, FwdIter2 dest,
            BinOp&& binary_op, UnOp&& unary_op)
        {
            static_assert(hpx::traits::is_forward_iterator<FwdIter1>::value,
                "Requires at least forward iterator.");
            static_assert(hpx::traits::is_forward_iterator<FwdIter2>::value,
                "Requires at least forward iterator.");

            return hpx::parallel::v1::detail::transform_inclusive_scan<
                FwdIter2>()
                .call(std::forward<ExPolicy>(policy), first, last, dest,
                    std::forward<UnOp>(unary_op),
                    std::forward<BinOp>(binary_op));
        }

        // clang-format off
        template <typename InIter, typename OutIter, typename BinOp,
            typename UnOp, typename T,
            HPX_CONCEPT_REQUIRES_(
                hpx::traits::is_iterator<InIter>::value &&
                hpx::traits::is_iterator<OutIter>::value &&
                hpx::is_invocable_v<UnOp,
                    typename std::iterator_traits<InIter>::value_type> &&
                hpx::is_invocable_v<BinOp,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<InIter>::value_type>::type,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<InIter>::value_type>::type
                >
            )>
        // clang-format on
        friend OutIter tag_fallback_dispatch(hpx::transform_inclusive_scan_t,
            InIter first, InIter last, OutIter dest, BinOp&& binary_op,
            UnOp&& unary_op, T init)
        {
            static_assert(hpx::traits::is_input_iterator<InIter>::value,
                "Requires at least input iterator.");
            static_assert(hpx::traits::is_output_iterator<OutIter>::value,
                "Requires at least output iterator.");

            return hpx::parallel::v1::detail::transform_inclusive_scan<
                OutIter>()
                .call(hpx::execution::seq, first, last, dest,
                    std::forward<UnOp>(unary_op), std::move(init),
                    std::forward<BinOp>(binary_op));
        }

        // clang-format off
        template <typename ExPolicy, typename FwdIter1, typename FwdIter2,
            typename BinOp, typename UnOp, typename T,
            HPX_CONCEPT_REQUIRES_(
                hpx::is_execution_policy<ExPolicy>::value &&
                hpx::traits::is_forward_iterator<FwdIter1>::value &&
                hpx::traits::is_forward_iterator<FwdIter2>::value &&
                hpx::is_invocable_v<UnOp,
                    typename std::iterator_traits<FwdIter1>::value_type> &&
                hpx::is_invocable_v<BinOp,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<FwdIter1>::value_type>::type,
                    typename hpx::util::invoke_result<UnOp,
                        typename std::iterator_traits<FwdIter1>::value_type>::type
                >
            )>
        // clang-format on
        friend typename parallel::util::detail::algorithm_result<ExPolicy,
            FwdIter2>::type
        tag_fallback_dispatch(hpx::transform_inclusive_scan_t,
            ExPolicy&& policy, FwdIter1 first, FwdIter1 last, FwdIter2 dest,
            BinOp&& binary_op, UnOp&& unary_op, T init)
        {
            static_assert(hpx::traits::is_forward_iterator<FwdIter1>::value,
                "Requires at least forward iterator.");
            static_assert(hpx::traits::is_forward_iterator<FwdIter2>::value,
                "Requires at least forward iterator.");

            return hpx::parallel::v1::detail::transform_inclusive_scan<
                FwdIter2>()
                .call(std::forward<ExPolicy>(policy), first, last, dest,
                    std::forward<UnOp>(unary_op), std::move(init),
                    std::forward<BinOp>(binary_op));
        }
    } transform_inclusive_scan{};
}    // namespace hpx
