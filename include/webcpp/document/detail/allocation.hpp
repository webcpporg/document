// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 What an allocator's failure is, and how an operation that allocates turns it into a value: with
 exceptions, an allocator fails by throwing `std::bad_alloc`, a type derived from it, or
 Boost.Interprocess's `bad_alloc`, which its allocators throw and which derives from
 `std::exception` alone. Anything else an allocator throws leaves a `noexcept` operation, and ends
 the program; without exceptions, an allocator that fails ends the program, as it does itself.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_ALLOCATION_HPP
#define WEBCPP_DOCUMENT_DETAIL_ALLOCATION_HPP

#include <webcpp/document/config.hpp>

#include <boost/interprocess/exceptions.hpp>  // lint-world: its allocators' failure, no segment

#include <new>
#include <utility>

namespace webcpp::document::detail {

/**
 Runs `run` and returns what it returns, or, with exceptions, what `failed` returns when an
 allocator fails in it: by throwing `std::bad_alloc`, a type derived from it, or
 Boost.Interprocess's `bad_alloc`. Anything else `run` throws propagates.

 @param run What allocates.
 @param failed What gives the result of a failure, of the type `run` returns.
 @return What `run` or `failed` returned.
*/
template <class Run, class Failed>
auto unless_allocation_fails(Run&& run, [[maybe_unused]] Failed&& failed)
    -> decltype(std::forward<Run>(run)()) {
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    try {
        return std::forward<Run>(run)();
    } catch (const std::bad_alloc&) {
        return failed();
    } catch (const boost::interprocess::bad_alloc&) {
        return failed();
    }
#else
    return std::forward<Run>(run)();
#endif
}

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_ALLOCATION_HPP
