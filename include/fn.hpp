/*
================================================================================
 
                             PUBLIC DOMAIN NOTICE
                 National Center for Biotechnology Information
 
  This software is a "United States Government Work" under the terms of the
  United States Copyright Act.  It was written as part of the author's official
  duties as a United States Government employees and thus cannot be copyrighted. 
  This software is freely available to the public for use. The National Library
  of Medicine and the U.S. Government have not placed any restriction on its use
  or reproduction.
 
  Although all reasonable efforts have been taken to ensure the accuracy and
  reliability of this software, the NLM and the U.S. Government do not and
  cannot warrant the performance or results that may be obtained by using this
  software. The NLM and the U.S. Government disclaim all warranties, expressed
  or implied, including warranties of performance, merchantability or fitness
  for any particular purpose.
 
  Please cite NCBI in any work or product based on this material.
 
================================================================================

  Author: Alex Astashyn

*/
#ifndef RANGELESS_FN_HPP_
#define RANGELESS_FN_HPP_

#include <stdexcept> // to include std::logic_error for MSVC
#include <algorithm>
#include <functional>
#include <vector>
#include <map>
#include <deque> // can we get rid of this and implement in terms of ring-buffer?
#include <string> // for to_string
#include <iterator> // for std::inserter, MSVC
#include <cassert>

#if defined(DOXYGEN) || (defined(RANGELESS_FN_ENABLE_RUN_TESTS) && RANGELESS_FN_ENABLE_RUN_TESTS)
#    define RANGELESS_FN_ENABLE_PARALLEL 1
#endif

// cost of #include <fn.hpp> without par-transform: 0.45s; with: 0.85s, so will disable by default
#if RANGELESS_FN_ENABLE_PARALLEL
#    include <deque>
#    include <future>
#    include <thread>
#endif

#if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable: 4068) // unknown pragmas (for GCC diagnostic push)
#endif


#define RANGELESS_FN_THROW(msg) throw std::logic_error( std::string{} + __FILE__ + ":" + std::to_string(__LINE__) + ": "#msg );

namespace rangeless
{

/// @brief LINQ -like library of higher-order functions for data manipulation.
namespace fn
{

namespace impl
{
    // A trick to make compiler produce a compilation error printing the expanded type.
    // Usage: TheTypeInQuestionIs<T>{}; or TheTypeInQuestionIs<decltype(expr)>{};
    // The compiler output will have ... TheTypeInQuestionIs<int> ...
    //                                                       ^^^ computed type
    template<typename...> struct TheTypeInQuestionIs;

    template<typename IteratorTag, typename Iterable>
    static inline void require_iterator_category_at_least(const Iterable&)
    {
        static_assert(std::is_base_of<IteratorTag, typename std::iterator_traits<typename Iterable::iterator>::iterator_category>::value, "Iterator category does not meet minimum requirements.");
    }

    /////////////////////////////////////////////////////////////////////////
    /// Very bare-bones version of std::optional-like with rebinding assignment semantics.
    template<class T>
    class maybe
    {
       struct sentinel{};
       union
       {
           sentinel m_sentinel;
                  T m_value;
       };

       bool m_empty = true;
     
    public:
        using value_type = T;

        static_assert(!std::is_same<value_type, void>::value, "Can't have void as value_type - did you perhaps forget a return-statement in your transform-function?");

        maybe() : m_sentinel{}
        {}

        // to avoid double-destruction
        maybe(const maybe&) = delete;
        maybe& operator=(const maybe&) = delete;

        maybe(T val)
        {
            reset(std::move(val));
        }

        maybe(maybe&& other) noexcept
        {        //^^ by value instead?
            if(!other.m_empty) {
                reset(std::move(*other));
                other.reset();
            }
        }

        // need this for monadic binding?
        // maybe(maybe<maybe<T>> other) noexcept { ... }


        // NB: the assignment semantics differ from that of std::optional!
        // See discussion in reset() below
        maybe& operator=(maybe&& other) noexcept
        {
            if(this == &other) {
                ;
            } else if(!other.m_empty) {
                reset(std::move(*other));
                other.reset();
            } else {
                reset();
            }
            return *this;
        }

        void reset(T val)
        {
            // NB: even if we are holding a value, we don't move-assign to it
            // and instead reset and place-new, because the type may be
            // move-constructible but not move-assigneable, e.g. containing a closure.
            // https://stackoverflow.com/questi ons/38541354/move-assignable-lambdas-in-clang-and-gcc/
            //
            // Another reason we can't simply move-assign to
            // is that T may be a tuple containing 
            // references, and we want to assign maybe<T> of such tuple
            // without assigning referenced objects as a side-effect,
            // unlike std::optional: (See NB[2])
            //     int x = 42;
            //     int y = 99;
            //     std::optional<std::tuple<int&>> opt_x = std::tie(x);
            //     std::optional<std::tuple<int&>> opt_y = std::tie(y);
            //     opt_x = opt_y; // x is now 99 - Why, c++ standard committee? whyyyy??
            //
            // For our purposes maybe<T> should behave similarly 
            // to a unique_ptr, except with stack-storage, where
            // reassignment simply transfers ownership.
            //
            // Related:
            // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3527.html#optional_ref.rationale.assign
            // https://www.fluentcpp.com/2018/10/05/pros-cons-optional-references/

            reset();
            
            new (&m_value) T(std::move(val));

            m_empty = false;
        }

        void reset()
        {
            if(!m_empty) {
                this->operator*().~T();
                m_empty = true;
            }
        }

        explicit operator bool() const noexcept
        {
            return !m_empty;
        }
     
        T& operator*() noexcept
        {
            assert(!m_empty);

#if __cplusplus >= 201703L
            return *std::launder(&m_value);
#else
            return m_value;
#endif
        }

        // I believe technically we need std::launder here, yet I couldn't
        // find a single implementation of `optional` that launders 
        // memory after placement-new, or side-steps the issue.
        //
        //      https://en.cppreference.com/w/cpp/types/aligned_storage (static_vector example)
        //      https://github.com/ericniebler/range-v3
        //      https://github.com/mpark/base
        //      https://github.com/akrzemi1/Optional
        //      https://github.com/TartanLlama/optional
        //      https://github.com/ned14/outcome (MVP for discussing the issue in the documentation)
        //
        // We'll formally test for expected behavior in a unit-test.

        const T& operator*() const noexcept
        {
            assert(!m_empty);
            
#if __cplusplus >= 201703L
            return *std::launder(&m_value);
#else
            return m_value;
#endif
        }

#if 0
        // this causes internal compiler error in MSVC
        template<typename F>
        auto transform(F fn) && -> maybe<decltype(fn(std::move(**this)))>
        {
            if(m_empty) {
                return { };
            }

            return { fn(std::move(**this)) };
        }
#endif
     
        ~maybe() 
        {
            reset();
        }
    };

    /////////////////////////////////////////////////////////////////////////////
    // Will be used to control SFINAE priority of ambiguous overload resolutions.
    // https://stackoverflow.com/questions/34419045

    struct pr_lowest {};
    struct pr_low     : pr_lowest {};
    struct pr_high    : pr_low    {};  
    struct pr_highest : pr_high   {};  

    using resolve_overload = pr_highest;


}   // namespace impl


    /// @defgroup io Inputs and Outputs
    ///
    /*!
    @code
      fn::seq([]{ ... }) % ... // as input-range from a nullary invokable
          std::move(vec) % ... // pass by-move
                    vec  % ... // pass by-copy
           fn::from(vec) % ... // as view yielding elements by-move (or copy-view, if vec is const)
          fn::cfrom(vec) % ... // as const view yielding elements by-copy (regardless of whether vec is const)
           fn::refs(vec) % ... // as seq yielding reference-wrappers
    @endcode
    */
    /// @{

    /////////////////////////////////////////////////////////////////////////
    /// @brief Return fn::end_seq() from input-range generator function to signal end-of-inputs.
    ///
    /// This throws fn::end_seq::exception on construction, and is interpreted internally as end-of-inputs.
    /// This exception does not propagate outside of the API's boundaries.
    struct end_seq
    {
        // First I called it just "end", but want to avoid name confusion, because
        // end() usually returns end-iterator in c++.

        /* I chose to allow exception-based approach to singal end-of-inputs.
         *
         * This is a pythonic approach to signal end-of-inputs from a generator function.
         * This is cleanest from the API perspective, allowing for very trivial 
         * custom logic with fn::adapt, and also most straightforward under the hood
         * implementation-wise. Using exceptions for flow control is generally
         * considered a bad idea, but since the API is exeception-neutral, the
         * only downside from the external perspective is the overhead
         * associated with exceptional code path).
         *
         * Update: Switched the internals to monadic end-of-seq signaling by passing values
         * via maybe-wrapper everywhere. To allow easy usage to user-code if they
         * can tolerate exception-handling overhead, the user-code gen-function is wrapped 
         * in catch_end callable that will catch the end_seq::exception and will return
         * empty-maybe, adapting the gen-function from exception-based to empty-maybe representation.
         */

        struct exception
        {};

        // TODO: can we preallocate exception with std::make_exception_ptr
        // and then reuse it with std::rethrow_exception, which would avoid 
        // allocation? This doesn't seem to make any difference on throughput, however.
        //
        // Related article on performance of exceptions:
        // http://nibblestew.blogspot.com/2017/01/measuring-execution-performance-of-c.html


        // throw on construction or in conversion? 
        // There are pros and cons, i.e.
        // a user may simply do `fn::end_seq();` 
        // and expect it to take effect.
        // 
        // Also,
        //      return cond ? ret : fn::end_seq();
        // will prevent copy-elision, one has to remember to std::move(ret).
        end_seq()
        {
            throw exception{};
        }

#if 0 // MSVC 19.15 does not support noreturn
        template<typename T>            
        [[ noreturn ]] operator T() const
        {
            throw exception{};
        }
#else
        template<typename T>
        operator T() const
        {
            throw exception{};
            return std::move(*impl::maybe<T>{});
        }
#endif

    };

    

namespace impl
{
    // will isolate exception-based end-of-seq signaling and will
    // not throw end_seq::exception ourselves (except in adapt,
    // if user-code calls gen() past-the-end) to allow nonthrowing functionality.
    template<typename Gen>
    struct catch_end
    {
        Gen gen;
        bool ended; // after first end_seq::exception
                    // will return empty-maybe without invoking gen, to avoid
                    // repeating the exception-handling overhead.

        using value_type = decltype(gen());

        auto operator()() -> maybe<value_type>
        {
            if(ended) {
                return { };
            }

            try {
                return { gen() };
            } catch( const end_seq::exception& ) {
                ended = true;
                return { };
            }
        }
    };


    /////////////////////////////////////////////////////////////////////
    // invoke gen.recycle(value) if gen defines this method
    template<typename G, typename T>
    auto recycle(G& gen, T& value, pr_high) -> decltype(gen.recycle(value), void())
    {
        gen.recycle(value);
    }

    // Low-priority fallback when gen.recycle(...) is not viable.
    template<typename G, typename T>
    void recycle(G&, T&, pr_low)
    {
        // no-op
    }

    /////////////////////////////////////////////////////////////////////////
    template<typename Gen>
    struct get_value_type
    {
        using type = typename Gen::value_type;
    };

    template<typename T>
    struct get_value_type<std::function<impl::maybe<T>()>>
    {
        using type = T;
    };


    /////////////////////////////////////////////////////////////////////////
    /// Single-pass InputRange-adapter for nullary generators.
    ///
    template<typename Gen>
    class seq 
    {
        // NB: This yields rvalue-references (`reference = value_type&&`)
        // Pros:
        // 1) If the downstream function takes arg by value (e.g. to transform, for_each, foldl, etc), 
        // it will be passed without making a copy.
        //
        // 2) We want to prevent user-code from making lvalue-references
        // to the iterator's value, that will be invalidated when the iterator is incremented.
        //
        // Cons: move-iterators are not widely used, and an expression like `auto x = *it;` 
        // leaves *it in moved-from state, and it may not be clear from the context that a move just happened.
        // That said, the entire point of this library is so that the user-code does not 
        // need to deal with iterators directly.
    
    public:
        using value_type = typename get_value_type<Gen>::type;
        static_assert(!std::is_reference<value_type>::value, "The type returned by the generator-function must be a value-type. Use std::ref if necessary.");

        seq(Gen gen) 
            : m_gen( std::move(gen) ) // NB: must use parentheses here!
        {}

        // To support conversion to any_seq_t.
        // (i.e. Gen is a std::function, and OtherGen is some-lambda-type)
        //
        // Used to implement as rvalue-specific implicit conversion,
        //      using any_seq_t = seq<std::function<impl::maybe<value_type>()>>;
        //      operator any_seq_t() && { ... }
        //
        // but can't do this because of MSVC bug
        // causing ambiguous overload resolution vs.
        //      operator std::vector<value_type>() &&
        template<typename OtherGen>
        seq(seq<OtherGen> other)
            :   m_current{ std::move(other.m_current) }
            ,       m_gen{ std::move(other.m_gen) }
            ,   m_started{ other.m_started }
            ,     m_ended{ other.m_ended }
            , m_resumable{ other.m_resumable }

        {
            other.m_started = true;
            other.m_ended   = true;
        }   

                   seq(const seq&) = delete;
        seq& operator=(const seq&) = delete;

                        seq(seq&&) = default; // TODO: need to customize to set other.m_ended = other.m_started = true?
             seq& operator=(seq&&) = default;

        /////////////////////////////////////////////////////////////////////
        class iterator
        {
        public:
            using iterator_category = std::input_iterator_tag;
            using   difference_type = void;
            using        value_type = seq::value_type;
            using           pointer = value_type*;
            using         reference = value_type&&; // NB: rvalue-reference

            iterator(seq* p = nullptr) : m_parent{ p }
            {}

            iterator& operator++()
            {
                if(!m_parent || m_parent->m_ended) {
                    m_parent = nullptr;
                    return *this;
                }

                auto& p = *m_parent;

                if(p.m_current) {
                    impl::recycle(p.m_gen, *p.m_current, resolve_overload{});
                }

                p.m_current = p.m_gen();

                if(!p.m_current) {
                    p.m_ended = true;
                    m_parent = nullptr; // reached end
                }
                return *this;
            }

#pragma GCC diagnostic push 
#pragma GCC diagnostic ignored "-Weffc++" // insists operator++(int) should be returning iterator
            maybe<value_type> operator++(int) // to support *it++ usage
            {
                auto ret = std::move(m_parent->m_current);
                this->operator++();
                return ret;
            }
#pragma GCC diagnostic pop


            reference operator*() const { return static_cast<reference>(*m_parent->m_current); }
            // Why do we need static_cast<reference> instead of simply std::move ?
            // See https://en.cppreference.com/w/cpp/iterator/move_iterator/operator*

            pointer operator->() const { return &*m_parent->m_current; }

            bool operator==(const iterator& other) const
            {
                return m_parent == other.m_parent;
            }

            bool operator!=(const iterator& other) const
            {
                return m_parent != other.m_parent;
            }

        private:

            friend seq;
           
            seq* m_parent;
        };

        iterator begin() // NB: non-const, advances to the first element, (calls m_gen)
        {
            // An input-range is single-pass. Not expecting multiple calls to begin()
            if(m_started && !m_resumable) {
                RANGELESS_FN_THROW("seq::begin() can only be called once per instance. Set resumable(true) to override.");
            }

            auto it = iterator{ m_ended ? nullptr : this };

            if(!m_started && !m_ended) {
                m_started = true; // advance to the first element (or end)
                ++it;
            }
            return it;
        }

        static iterator end()
        {
            return {};
        }

#if 0
        // not providing this because begin() may be once-callable

        bool empty() // nb: non-const because begin() is non-const.
        {            // should this be made const (make fields mutable) ?
            //return m_ended; // can't do that - may be empty, but m_Advanced==fase
            return begin() == end();
        }
#endif

        /// By default, calling begin() a second time will throw, because
        /// the default expectation is that begin() will restart from the beginning,
        /// which does not hold with seq, or any input-range for that matter. 
        /// This may be suppressed with .resumable(true),
        /// making it explicit that the user-code is aware that begin() will resume
        /// from the current state.
        seq& set_resumable(bool res = true)
        {
            m_resumable = res;
        }

        Gen& get_gen()
        {
            return m_gen;
        }

        const Gen& get_gen() const
        {
            return m_gen;
        }

        operator std::vector<value_type>() && // rvalue-specific because after conversion
        {                                     // the seq will be consumed (m_ended)
            assert(!m_ended);
            //assert(!m_started); allow or prohibit?

            std::vector<value_type> ret{};

            if(m_current) {
                ret.push_back(std::move(*m_current));
                m_current.reset();
            }

            for(auto x = m_gen(); x; x = m_gen()) {
                ret.push_back(std::move(*x)); 
            }

            m_started = true;
            m_ended = true;

            return ret;
        }


    private: 
       friend class iterator;

       // so that we can access private fields in constructor taking seq<OtherGen>
       template<typename U>
       friend class seq;

       impl::maybe<value_type> m_current = {};  
                             // Last value returned by m_gen.
                             // Can't be simply T because T
                             // might not be default-constructible.

            Gen m_gen;               // nullary generator
           bool m_started   = false; // false iff did not advance to begin()
           bool m_ended     = false;
           bool m_resumable = false; // if true, do not throw if called begin() more than once.
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Iterable>
    struct refs_gen
    {    
        Iterable& cont; // maybe const

        using value_type = decltype(std::ref(*cont.begin()));
        // std::reference_wrapper< maybe-const Iterable::value_type>

        decltype(cont.begin()) it; // may be iterator or const_iterator

        auto operator()() -> maybe<value_type>
        {    
            if(it == cont.end()) {
                return { }; 
            } else {
                return { *it++ };
            }    
        }    
    };   

}   // namespace impl

    /////////////////////////////////////////////////////////////////////////
    /// @brief Adapt a generator function as `InputRange`.
    /*!
    @code
        int i = 0;
        int sum =
            fn::seq([&i]
            {
                return i < 5 ? x++ : fn::end_seq();
            })

          % fn::where([](int x)
            {
                return x > 2;
            }) // 3, 4

          % fn::transform([](int x)
            {
                return x + 1;
            }) // 4, 5

          % fn::foldl_d([](int out, int in)
            {
                return out * 10 + in;
            });
                
        VERIFY(sum == 45);
    @endcode
    */
    template<typename NullaryInvokable>
    impl::seq<impl::catch_end<NullaryInvokable>> seq(NullaryInvokable gen_fn)
    {
        static_assert(!std::is_reference<decltype(gen_fn())>::value, "The type returned by gen_fn must be a value-type.");
        static_assert(!std::is_same<decltype(gen_fn()), void>::value, "You forgot a return-statement in your gen-function.");
        return { { std::move(gen_fn), false } };
    }
 
    /////////////////////////////////////////////////////////////////////////
    /// @brief Adapt a reference to `Iterable` as `seq` yielding reference-wrappers.
    template<typename Iterable>
    impl::seq<impl::refs_gen<Iterable>> refs(Iterable& src)
    {
        // Prevent usage with input-ranges, since reference-wrappers will become dangling
        impl::require_iterator_category_at_least<std::forward_iterator_tag>(src);

        return { { src, src.begin() } };
    }

    /////////////////////////////////////////////////////////////////////////
    /// @brief Type-erase a `seq`.
    ///
    /// `any_seq_t` is implicitly constructible from any `seq`,
    /// wrapping the underlying `NullaryInvokable` as `std::function`.
    ///
    /// This may be useful when you want to declare a function or a method
    /// returning a seq, but don't want to define it with all the gory details
    /// in the header.
    ///
    /// NB: wrappnig a callable in a `std::function` is subject to
    /// performance-related considerations.
    /*!
    @code
    fn::any_seq_t<int> myseq = 
        fn::seq([i = 0]() mutable
        {
            return i < 10 ? i++ : fn::end_seq();
        })
      % fn::where([](int x)
        {
            return x % 2 == 0;
        });
    @endcode
    */
    template<typename T>
    using any_seq_t = impl::seq<std::function<impl::maybe<T>()>>;
    /// @}
   
    
namespace impl
{
    /////////////////////////////////////////////////////////////////////
    // User-provided key_fn may be returning a std::reference_wrapper<...>.
    // So we'll be comparing keys using lt or eq, equipped to deal with that.
    // See also https://stackoverflow.com/questions/9139748/using-stdreference-wrapper-as-the-key-in-a-stdmap

    struct lt
    {

        template<typename T>
        bool operator()(const T& a, const T& b) const
        {
            return std::less<T>{}(a, b);
        }

        template<typename T>
        bool operator()(const std::reference_wrapper<T>& a, 
                        const std::reference_wrapper<T>& b) const
        {
            return std::less<T>{}(a.get(), b.get());
        }
    };

    struct eq
    {
        template<typename T>
        bool operator()(const T& a, const T& b) const
        {
            return a == b;
        }

        template<typename T>
        bool operator()(const std::reference_wrapper<T>& a, 
                        const std::reference_wrapper<T>& b) const
        {
            return a.get() == b.get();
        }
    };

    template<typename T>
    int compare(const T& a, const T&b)
    {
        return ( lt{}(a, b) ? -1 
               : lt{}(b, a) ?  1
               :               0);
    }


    /////////////////////////////////////////////////////////////////////
    // @brief Value-wrapper with reversed operator<, used with by::decreasing
    template<typename T>
    struct gt
    {
        T val; // this has to be non-const, otherwise does not compile with T=bool (or int, etc)
               // when used as part of a tuple (e.g. in the big example
               // make_tuple(fn::by::decreasing(is_nc), ...); // compilation error if const T val; here
               // make_tuple(!is_nc, ...); // ok
               // 
               // Having a const field also prevents the use of move-constructor
               // when passing by move.

        bool operator<(const gt& other) const
        {
            return std::less<T>{}(other.val, this->val);
        }

        // operator== in case it will be used with group_adjacent_by or unique_adjacent_by
        bool operator==(const gt& other) const
        {
            return other.val == this->val;
        }
    };

    // composition of gt over key_fn
    template<typename F>
    struct gt_fn
    {
        const F key_fn;

        template<typename Arg>
        auto operator()(const Arg& arg) const -> gt<decltype(key_fn(arg))>
        {
            return { key_fn(arg) };
        }
    };

    // binary comparator composed over key_fn
    template<typename F>
    struct comp
    {
        const F key_fn;

        // NB: may be type-assymetric
        template<typename A, typename B>
        bool operator()(const A& a, const B& b) const
        {
            return lt{}(key_fn(a), key_fn(b));
        }
    };

}   // namespace impl


/// @brief Common key-functions to use with sort_by/unque_by/group_all_by
/// @code
/// ptrs = %= fn::sort_by(fn::by::dereferenced{});
/// std::move(pairs) % fn::group_all_by(fn::by::second{});
/// @endcode
namespace by
{
    // sort() / unique() are simply sort_by<identity> and unique_adjacent_by<identity>
    struct identity 
    {
        template<typename T>
        auto operator()(const T& x) const -> const T&
        {
            return x;
        }
    };

    struct dereferenced
    {
        template<typename P> // any dereferenceable type
        auto operator()(const P& ptr) const -> decltype(*ptr)
        {
            return *ptr;
        }
    };

    struct first
    {
        template<typename T>
        auto operator()(const T& x) const -> decltype(*&x.first) // *& so that decltype computes a reference
        {
            return x.first;
        }
    };

    struct second
    {
        template<typename T>
        auto operator()(const T& x) const -> decltype(*&x.second)
        {
            return x.second;
        }
    };

    /// e.g. for tuples or pairs, `fn::group_adjacent_by(fn::by::get<string>{})`
    template<typename U>
    struct get
    {
        template<typename T>
        auto operator()(const T& x) const -> const U&
        {
            return std::get<U>(x);
        }
    };

    /// @brief Wraps the passed value and exposes inverted operator<.
    /*!
    @code
    {{
        // sort by longest-first, then lexicographically

        const auto inp      = std::vector<std::string>{ "2", "333", "1", "222", "3" };
        const auto expected = std::vector<std::string>{ "222", "333", "1", "2", "3" };


        auto ret1 = inp % fn::sort_by([](const string& s)
        {
            return std::make_tuple(fn::by::decreasing(s.size()), std::ref(s));
        });
        VERIFY(ret1 == expected);


        auto ret2 = inp % fn::sort_by([](const string& s)
        {
            return std::make_tuple(s.size(), fn::by::decreasing(std::ref(s)));
        }) % fn::reverse();
        VERIFY(ret2 == expected);


        // we can also compose by::decreasing with a key-function
        auto ret3 = inp % fn::sort_by(fn::by::flipped([](const std::string& s)
        {
            return std::make_tuple(s.size(), fn::by::decreasing(std::ref(s)));
        }));
        VERIFY(ret3 == expected);


        // we can also create a comparator from a key-function.
        auto ret4 = inp;
        gfx::timsort(ret4.begin(), ret4.end(), fn::by::make_comp([](const std::string& s)
        {
            return std::make_tuple(fn::by::decreasing(s.size()), std::ref(s));
        }));
        VERIFY(ret4 == expected);


        // NB: we can't use std::tie because fn::by::decreasing returns an rvalue,
        // so we use std::make_tuple and capture s by std::ref.
    }}
    @endcode
    */
    template<typename T>
    impl::gt<T> decreasing(T x)
    {
        // Note: we could perfect-forward T instead of taking it by value, 
        // such that return type is gt<const reference>,
        // but this would allow misuse such as
        // fn::sorty_by([](const auto& x) 
        // {
        //      const auto key = x.get_key();
        //      return fn::by::decreasing(key);
        //      // if key is captured by reference,
        //      // it becomes dangling after return.
        // });  
        // Instead, will allow the user to explicitly 
        // capture by-reference via reference-wrapper below.

        return { std::move(x) };
    }

    template<typename T>
    impl::gt<const T&> decreasing(std::reference_wrapper<T> x)
    {
        // unwrapping reference_wrapper and capturing by const-reference,
        // because otherwise unwrapped reference_wrapper won't bind to
        // operator< in gt::operator<.
        return { x.get() };
    }

    /// @brief Compose key_fn with `by::decreasing`.
    template<typename F>
    impl::gt_fn<F> flipped(F key_fn)
    {
        return { std::move(key_fn) };
    }

    /// @brief Make binary comparison predicate from a key-function
    template<typename F>
    impl::comp<F> make_comp(F key_fn)
    {
        return { std::move(key_fn) };
    }
}   // namespace by

/// Common transform-functions that can be used as param to fn::transform
namespace get
{
    struct dereferenced
    {
        template<typename P> // any dereferenceable type
        auto operator()(P ptr) const -> typename std::remove_reference<decltype(std::move(*ptr))>::type
        {
            return std::move(*ptr);
        }
    };

    struct first
    {
        template<typename T>
        auto operator()(T x) const -> typename T::first_type
        {
            return std::move(x.first);
        }
    };

    struct second
    {
        template<typename T>
        auto operator()(T x) const -> typename T::second_type
        {
            return std::move(x.second);
        }
    };

    struct enumerated
    {
        size_t i = 0UL;

        template<typename T>
        auto operator()(T x) -> std::pair<size_t, T>
        {
            return { i++, std::move(x) };
        }
    };
}

    /// @defgroup view Views
    /// @{

    /////////////////////////////////////////////////////////////////////////
    /// @brief A view is just a pair of interators with begin() and end() interface.
    template<typename Iterator>
    class view
    {
    private:
        Iterator it_beg;
        Iterator it_end;

    public:
        view(Iterator b, Iterator e)
            : it_beg( std::move(b) )
            , it_end( std::move(e) )
        {}

        view() = default;

        using iterator = Iterator;
        using value_type = typename iterator::value_type;
        
        Iterator begin() const { return it_beg; }
        Iterator end()   const { return it_end; }

        /// Truncate the view.
        ///
        /// Precondition: `b == begin() || e = end()`; throws `std::logic_error` otherwise.
        /// This does not affect the underlying range.
        void erase(Iterator b, Iterator e)
        {
            // We support the erase method to obviate view-specific overloads 
            // for some hofs, e.g. take_while, take_first, drop_whille, drop_last, etc -
            // the container-specific overloads will work for views as well.

            if(b == it_beg) {
                // erase at front
                it_beg = e;
            } else if(e == it_end) {

                // erase at end
                impl::require_iterator_category_at_least<std::forward_iterator_tag>(*this);

                it_end = b;
            } else {
                RANGELESS_FN_THROW("Can only erase at the head or at the tail of the view");
            }
        }

        void clear()
        {
            it_beg = it_end;
        }

        bool empty() const
        {
            return it_beg == it_end;
        }
    };

    // Note: we named the methods from, such that usage like
    //     auto sorted_vec = fn::from(it_beg, it_end) % fn::sort();
    // communicates that the range will be moved-from.

    /// @brief Create a range-view from a pair of iterators.
    template<typename Iterator>
    constexpr view<Iterator> from(Iterator it_beg, Iterator it_end) noexcept
    {
        return { std::move(it_beg), std::move(it_end) };
    }

    /// To enable composability of APIs returning a pair of iterators, e.g. std::equal_range
    template<typename Iterator>
    constexpr view<Iterator> from(std::pair<Iterator, Iterator> p) noexcept
    {
        return { std::move(p.first), std::move(p.second) };
    }

    /// Create a range-view for a container, or an iterable that has `begin` and `end` as free functions rather than methods.
    template<typename Iterable,
             typename Iterator = typename Iterable::iterator>
    constexpr view<Iterator> from(Iterable& src) noexcept
    {
        using std::begin;
        using std::end;
        return { begin(src), end(src) };
    }

    template<typename Iterable,
             typename Iterator = typename Iterable::const_iterator>
    constexpr view<Iterator> from(const Iterable& src) noexcept
    {
        // need a separate overload for const, because without it
        // begin(src) yields const_iterator that is not convertible
        // to normal iterator that's in the signature.

        using std::begin; // cbegin/cend here instead?
        using std::end;
        return { begin(src), end(src) };
    }


    template<typename Iterable,
             typename Iterator = typename Iterable::const_iterator>
    constexpr view<Iterator> cfrom(const Iterable& src) noexcept
    {
        using std::begin;
        using std::end;
        return { begin(src), end(src) };
    }

    /// @}



/////////////////////////////////////////////////////////////////////////
/// @brief Implementations for corresponding static functions in fn::
namespace impl
{   
    // Wrap an Iterable as gen-callable, yielding elements by move.
    struct to_seq
    {
        /////////////////////////////////////////////////////////////////////////
        // We compose seqs by yanking m_gen from the input seq,
        // wrapping it into a nullary callable (see gen) that will do the
        // additional work (e.g. filter, transform, etc) and wrapping back as seq. 
        // I.e. we're doing the monadic "burrito" technique.
        // See RANGELESS_FN_OVERLOAD_FOR_SEQ
        // 
        // The alternative is to wrap any range with adapting seq, but
        // this results in a to_seq at every layer instead of the outermost one.
        // 
        // So if we want to work with a user-provided input-range or a container lazily,
        // we first need to wrap it as seq that will yield elements by-move.
        template<typename Iterable>
        struct gen
        {
            using value_type = typename Iterable::value_type;
            using iterator   = typename Iterable::iterator;

            Iterable src_range;
            iterator it;
                bool started;

            auto operator()() -> maybe<value_type>
            {
                if(!started) {
                    started = true;
                    it = src_range.begin(); 
                        // r might not be a container (i.e. some input-range
                        // and begin()  may be non-const, so deferring until
                        // the first call to operator() rather than
                        // initializing it{ r.begin() } in constructor.
                } else if(it == src_range.end()) {
                    ; // may be equal to end in case of repeated calls to operator() after ended
                } else {
                    ++it;
                }

                if(it == src_range.end()) {
                    return { };
                } else {
                    return { std::move(*it) };
                }
            }
        }; 

        // pass-through if already a seq
        template<typename Gen>
        seq<Gen> operator()(seq<Gen> seq) const
        {
            return seq;
        }

        /// @brief Wrap a range (e.g. a container or a view) as seq.
        template<typename Iterable>
        seq<gen<Iterable>> operator()(Iterable src) const
        {
            return { { std::move(src) , {}, false } };
        }
    };


    /////////////////////////////////////////////////////////////////////
    struct to_vector
    {
        // passthrough overload
        template<typename T>
        std::vector<T> operator()(std::vector<T> vec) const
        {
            return vec;
        }

        // overload for a seq - invoke rvalue-specific implicit conversion
        template<typename Gen,
                 typename Vec = std::vector<typename seq<Gen>::value_type>>
        Vec operator()(seq<Gen> r) const
        {
            return static_cast<Vec>(std::move(r));
        }

        // overload for other iterable: move-insert elements into vec
        template<typename Iterable,
                 typename Vec = std::vector<typename Iterable::value_type> >
        Vec operator()(Iterable src) const
        {
            // Note: this will not compile with std::set
            // in conjunction with move-only value_type because 
            // set's iterators are const, and std::move will try 
            // and fail to use the copy-constructor.

            return this->operator()( Vec{
                    std::make_move_iterator(src.begin()),
                    std::make_move_iterator(src.end()) });
        }

        // an overload for map that will return a vector of
        // std::vector<std::pair<Key,       Value>>> rather than 
        // std::vector<std::pair<const Key, Value>>>, which is map's
        // value_type.
        // We want to remove the const because otherwise the vector's
        // functionality is crippled because the value-type is not 
        // move-assigneable, so we can't do anything useful with it.
        //
        // TODO: generalize this for sets and arbitrary associative containers,
        // pre- and post- c++17.
        template<typename Key, 
                 typename Value,
                 typename Vec = std::vector<std::pair<Key, Value>>>
        Vec operator()(std::map<Key, Value> m) const
        {
            Vec v{};
            v.reserve(m.size());

#if __cplusplus >= 201703L
            while(!m.empty()) {
                auto h = m.extract(m.begin());
                v.emplace_back(std::move(h.key()),
                               std::move(h.mapped()));
            }
#else
            for(auto it = m.begin(), it_end = m.end(); 
                     it != it_end;
                     it = m.erase(it))
            {
                v.emplace_back(std::move(it->first), // this makes a copy due to const key
                               std::move(it->second));
            }
#endif
            return v;
        }
    };


    template<typename Container>
    struct to
    {
        Container dest;

        // pass-through if dest is empty and same type.
        Container operator()(Container src) && // rvalue-specific because dest will be moved-from
        {
            if(dest.empty()) {
                return std::move(src);
            }

            for(auto&& x : src) {
                dest.insert(dest.end(), std::move(x));
            }       
            return std::move(dest);
        }


        template<typename Iterable>
        Container operator()(Iterable src) && // rvalue-specific because dest will be moved-from
        {
#if 0
            // won't compile with std::set
            dest.insert(dest.end(),
                        std::make_move_iterator(src.begin()),
                        std::make_move_iterator(src.end()));
#else
            for(auto&& x : src) {
                dest.insert(dest.end(), std::move(x));
            }       
#endif
            return std::move(dest);
        }
    };


    struct counts
    {
        template<typename Iterable>
        std::map<typename Iterable::value_type, size_t> operator()(Iterable&& xs) const
        {
            auto ret = std::map<typename Iterable::value_type, size_t>{};
            for(auto&& x : xs) {
                ++ret[x];
            }
            return ret;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Pred>
    struct exists_where
    {
        const Pred pred;
              bool ret;

        // e.g. bool ret = vals % !fn::exists_where(pred);
        exists_where operator!() &&
        {
            // don't want to make operator! non-const for lvalues,
            // so instead making it rvalue-specific to prevent
            // any misuse.

            ret = !ret;
            return std::move(*this);
        }

        template<typename Iterable>
        bool operator()(const Iterable& iterable) const
        {
            for(const auto& x : iterable)
                if(pred(x)) 
                    return ret;
            return !ret;
        }
    };

    // NB[3]: In for_each, foldl, foldl_d, foldl_1 we don't really need
    // the overloads for seq, because seq is Iterable, but by providing
    // them we work with underlying gens directly, bypassing the iterator
    // machinery layer of seq, potentially allowing the compiler to do 
    // better job with optimization.

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct for_each
    {
        F fn; // may be non-const, hence operator()s are also non-const.

        template<typename Iterable>
        void operator()(Iterable&& src)
        {
            for(auto it = src.begin(); it != src.end(); ++it) {
                fn(*it);
            }

            // Used to return by perfect-forwarding, i.e. 
            // return-type = Iterable&&, and return std::forward<Iterable>(src);
            //
            // The intent was to allow downstream stages
            // after for_each. However,
            // 1) If Iterable is a single-pass seq, 
            //    it's not usable after the loop (will cause runtime error
            //    if we try to iterate over it again).
            // 2) -Weffc++ says "should return by value" when 
            //    Iterable is passed by rvalue.
            // 
            // So instead, will treat for_each as lfold-into-void.
        }


        // See NB[3]
        template<typename Gen>
        void operator()(seq<Gen> src)
        {
            for(auto x = src.get_gen()(); x; x = src.get_gen()()) {
                fn(std::move(*x));
                impl::recycle(src.get_gen(), *x, impl::resolve_overload{});
            }
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Ret, typename Op>
    struct foldl
    {
        Ret init;
        Op fold_op;

        template<typename Iterable>
        Ret operator()(Iterable&& src) && // rvalue-specific because init will be moved-from
        {                                 // (we have to, because Ret may be move-only)

            static_assert(std::is_same<Ret, decltype(fold_op(std::move(init), *src.begin()))>::value, 
                         "Type of Init must be the same as the return-type of F");
    
            for(auto it = src.begin(); it != src.end(); ++it) {
                init = fold_op(std::move(init), *it);
            }

            // NB: can't use std::accumulate because it does not do the std::move(init)
            // internally until c++20, so it won't compile with move-only types, 
            // and will make copies for copyable types.

            return std::move(init);
        }

        // See NB[3]
        template<typename Gen>
        Ret operator()(seq<Gen> src) && 
        {
            for(auto x = src.get_gen()(); x; x = src.get_gen()()) {
                init = fold_op(std::move(init), std::move(*x));
                impl::recycle(src.get_gen(), *x, impl::resolve_overload{});
            }
            return std::move(init);
        }
    };
            
    /////////////////////////////////////////////////////////////////////
    template<typename Op>
    struct foldl_d
    {
        Op fold_op;

        struct any
        {
            template<typename T>
            operator T() const;
        };

        template<typename Iterable>
        auto operator()(Iterable&& src) const -> decltype(fold_op(any(), *src.begin()))
        {
            using ret_t = decltype(fold_op(any(), *src.begin()));
            auto ret = ret_t{};

            for(auto it = src.begin(); it != src.end(); ++it) {
                ret = fold_op(std::move(ret), *it);
            }

            return ret;
        }


        // See NB[3]
        template<typename Gen>
        auto operator()(seq<Gen> src) const -> decltype(fold_op(any(), std::move(*src.get_gen()())))
        {
            using ret_t = decltype(fold_op(any(), std::move(*src.get_gen()())));
            auto ret = ret_t{};

            for(auto x = src.get_gen()(); x; x = src.get_gen()()) {
                ret = fold_op(std::move(ret), std::move(*x));
                impl::recycle(src.get_gen(), *x, impl::resolve_overload{});
            }

            return ret;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct foldl_1
    {
        F fold_op;

        template<typename Iterable>
        auto operator()(Iterable&& src) const -> decltype(fold_op(std::move(*src.begin()), *src.begin()))
        {
            auto it = src.begin();
            auto it_end = src.end();

            if(it == it_end) {
                RANGELESS_FN_THROW("Expected nonempty.");
            }

            auto init = *it;

            for(++it; it != it_end; ++it) {
                init = fold_op(std::move(init), *it);
            }

            return init;
        }

        // See NB[3]
        template<typename Gen>
        auto operator()(seq<Gen> src) const -> decltype(fold_op(std::move(*src.get_gen()()), std::move(*src.get_gen()())))
        {
            auto x1 = src.get_gen()();

            if(!x1) {
                RANGELESS_FN_THROW("Expected nonempty.");
            }
            
            auto init = std::move(*x1);

            for(auto x = src.get_gen()(); x; x = src.get_gen()()) {
                init = fold_op(std::move(init), std::move(*x));
            }

            return init;
        }
    };

    /////////////////////////////////////////////////////////////////////////

    // define operator() overload composing an input-seq
    // (gen in OutGen presumed to go first)
    // Basically, we unwrap the input seq, yanking its gen,
    // compose our gen over it, and wrap it back into seq.
#define RANGELESS_FN_OVERLOAD_FOR_SEQ(...)                                 \
    template<typename InGen>                                               \
    auto operator()(seq<InGen> in) const -> seq<gen<InGen>>                \
    {                                                                      \
        return { { std::move(in.get_gen()), __VA_ARGS__ } };               \
    }                                                                

    // Default implementation of operator() for cases where 
    // there's no eager logic for Container-arg, and
    // we want to treat it the same as seq (overload above)
    // so we wrap Cont as to_seq::gen and do same as above.
    //
#define RANGELESS_FN_OVERLOAD_FOR_CONT(...)                                \
    template<typename Cont>                                                \
    auto operator()(Cont cont) const -> seq<gen<to_seq::gen<Cont>>>        \
    {                                                                      \
        return { { { std::move(cont), { }, false }, __VA_ARGS__ } };       \
    }                                                                      \
    //                                ^^^^^^^^^^
    // NB: it would be better, of course, to specify that `{ }` and `false`
    // as defaults in declarations of to_seq::gen fields instead of specifying them here,
    // but GCC-4.9.3 has problems with aggregate initialization in a presence
    // of field-defaults.


#define RANGELESS_FN_OVERLOAD_FOR_VIEW(...)                                \
    template<typename Iterator>                                            \
    auto operator()(view<Iterator> v) const                                \
     -> seq<gen<to_seq::gen<view<Iterator>>>>                              \
    {                                                                      \
        return { { { std::move(v), { }, false }, __VA_ARGS__ } };          \
    }    

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct transform
    {
        F map_fn;

        template<typename InGen>
        struct gen
        {
            InGen gen;
                F map_fn;

            using value_type = decltype(map_fn(std::move(*gen())));

            static_assert(!std::is_same<value_type, void>::value, "You forgot a return-statement in your transform-function.");

            auto operator()() -> maybe<value_type>
            {
                // NB: passing map_fn as ref, as it may be stateful (e.g. counting)
                //return gen().transform(std::ref(map_fn));

                auto x = gen();
                if(!x) {
                    return { };
                }

                auto ret = map_fn(std::move(*x));
                impl::recycle(gen, *x, impl::resolve_overload{});
                return std::move(ret); // I'd expect copy elision here, but
                                       // GCC4.9.3 tries to use copy-constructor here
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( map_fn )
        RANGELESS_FN_OVERLOAD_FOR_CONT( map_fn )
        // Given a container, we return a lazy seq rather than
        // transforming all elements eagerly, because inputs may be
        // "small", while outputs may be "large" in terms of 
        // memory and/or computation, so we want to defer it.
    };


    /////////////////////////////////////////////////////////////////////
    struct sliding_window
    {
        const size_t win_size;

        template<typename Iterable>
        struct gen_c
        {
            using iterator = typename Iterable::iterator;
            using value_type = view<iterator>;

                Iterable cont;
                iterator win_beg;
                iterator win_end;
                    bool started;
            const size_t win_size;

            auto operator()() -> maybe<value_type>
            {                
                if(!started) {
                    started = true;

                    win_beg = cont.begin();
                    win_end = cont.begin();

                    // advance view's it_end by win_size
                    size_t n = 0;
                    for(size_t i = 0; i < win_size && win_end != cont.end(); ++i) {
                        ++win_end;
                        ++n;
                    }

                    if(n < win_size) {
                        return { };
                    } else {
                        return value_type{ win_beg, win_end };
                    }
                }

                if(win_end == cont.end()) {
                    return { };
                }

                ++win_beg;
                ++win_end;

                return { { win_beg, win_end } };
            }
        };

        template<typename Iterable>
        auto operator()(Iterable inps) const -> seq<gen_c<Iterable>>
        {
            impl::require_iterator_category_at_least<std::forward_iterator_tag>(inps);

            return { { std::move(inps), 
                       {}, {}, // iteratos
                       false,  // started
                       win_size } };
        }

        /////////////////////////////////////////////////////////////////////

        // for seq: buffer win_size elements in a queue
        template<typename InGen>
        struct gen
        {
                   InGen gen;

            using inp_t      = typename InGen::value_type;
            using queue_t    = std::deque<inp_t>;
            using value_type = view<typename queue_t::iterator>;

                 queue_t queue;
              value_type curr;
            const size_t win_size;

            auto operator()() -> maybe<value_type>
            {                
                if(!queue.empty()) {
                    queue.pop_front();
                }

                while(queue.size() < win_size) {
                    auto x = gen();
                    if(!x) {
                        return { };
                    }
                    queue.push_back(std::move(*x));
                }

                return { { queue.begin(), queue.end() } };
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( {}, {{}, {}}, win_size )
    };

#if RANGELESS_FN_ENABLE_PARALLEL 

    // Default implementation of Async for par_transform below.
    struct std_async
    {
        template<typename NullaryCallable>
        auto operator()(NullaryCallable gen) const -> std::future<decltype(gen())>
        {
            return std::async(std::launch::async, std::move(gen));
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F, typename Async>
    struct par_transform
    {
          const Async async;
              const F map_fn;    // const, because expecting this to be thread-safe
               size_t queue_cap; // 0 means in-this-thread.

        par_transform& queue_capacity(size_t cap) 
        {
            queue_cap = cap;
            return *this;
        }

        template<typename InGen>
        struct gen
        {
                    InGen gen;
              const Async async;
                  const F map_fn;
             const size_t queue_cap;

            // TODO: with some handwaving may be able to use std::vector
            using value_type = decltype(map_fn(std::move(*gen())));
            using queue_t    = std::deque<std::future<value_type>>;

                  queue_t queue;

            auto operator()() -> maybe<value_type>
            {
                // behave like a plain impl::transform in in-this-thread mode
                if(queue_cap == 0) {
                    assert(queue.empty());
                    auto x = gen();
                    if(!x) {
                        return { };
                    } else {
                        return map_fn(std::move(*x));
                    }
                }

                struct job_t // wrapper for invoking fn on inp, passed by move
                {
                    using input_t = typename InGen::value_type;

                    const F& fn;
                     input_t inp; 

                    auto operator()() -> decltype(fn(std::move(inp)))
                    {    
                        return fn(std::move(inp));
                    }    
                };

                // if have more inputs, top-off the queue with async-tasks.
                while(queue.size() < queue_cap) {
                    auto x = gen();
                    
                    if(!x) {
                        break;
                    }

                    queue.emplace_back( async( job_t{ map_fn, std::move(*x) }));
                }

                if(queue.empty()) {
                    return { };
                }

                auto ret = queue.front().get(); // this returns by-value
                queue.pop_front();
                return { std::move(ret) };
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ(  async, map_fn, queue_cap, {} )
        RANGELESS_FN_OVERLOAD_FOR_CONT( async, map_fn, queue_cap, {} )
    };
#endif

    /////////////////////////////////////////////////////////////////////

#if 0
    Note: leaving the original implementation for reference.
    template<typename F>
    struct adapt
    {
        F fn;

        template<typename Gen>
        struct gen
        {
            Gen gen; // upstream nullary generator
            F fn;    // unary user-function taking the generator (NOT the result)

            auto operator()() -> decltype(fn(gen))
            {
                // fn may be taking gen by value [](auto gen){...},
                // so the state of the original (possibly mutable) gen 
                // will not be updated. We force passing by-reference here
                // with reference-wrapper, such that the above construct
                // in the user code invokes the original gen.
                return fn(std::ref(gen));
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( fn )
        RANGELESS_FN_OVERLOAD_FOR_CONT( fn )
    };
#else

    /////////////////////////////////////////////////////////////////////
    // The implementation below was added to enable the user-code (fn)
    // interrogate the passed gen whether it can yield another value
    // (so the user-code can wrap-up without throwing), so it prefetches
    // the next value from in_gen, and the gen-wrapper conversion to 
    // bool is false iff the prefetched maybe<...> is empty.
    //
    // The alternative is to have gen return a maybe<...>,
    // but we want to isolate the user-code from 
    // implementation details.
    template<typename F>
    struct adapt
    {
        F fn;

        template<typename InGen>
        struct gen
        {
                   InGen in_gen; // upstream nullary generator
                       F fn;     // unary user-function taking the generator (NOT the result)

            using inp_t = typename InGen::value_type;

            maybe<inp_t> inp;
                    bool started;

            // the gen-wrapper that will be passed to fn
            struct gen_wr
            {
                gen* parent;

                auto operator()() -> inp_t
                {
                    assert(parent);

                    if(!parent->inp) {
                        // user-code called gen() too many times
                        throw end_seq::exception{};
                    }

                    auto ret = std::move(*parent->inp);
                    parent->next();
                    return ret;
                }

                explicit operator bool() const
                {
                    assert(parent);

                    return bool(parent->inp);
                }
            };

            void next()
            {
                inp = in_gen();
            }

            using value_type = decltype(fn(gen_wr{ nullptr }));

            auto operator()() -> maybe<value_type>
            {
                if(!started) {
                    started = true;
                    next();
                }

                // user-function fn may throw end-of-inputs,
                // either explicitly or by invoking gen after
                // its conversion to bool equals false. We
                // catch it here and return empty-maybe exactly
                // as we do in catch_end-wrapper.
                try { 
                    return { fn(gen_wr{ this }) };

                } catch( const end_seq::exception& ) {
                    return { };
                }
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ(  fn, {}, false )
        RANGELESS_FN_OVERLOAD_FOR_CONT( fn, {}, false )
    };

#endif // adapt
   
    /////////////////////////////////////////////////////////////////////
    template<typename Pred>
    struct take_while
    {
        Pred pred;

        template<typename InGen>
        struct gen
        {
            InGen gen;
             Pred pred;
             bool found_unsatisfying;

            using value_type = typename InGen::value_type;

            auto operator()() -> maybe<value_type>
            {
                if(found_unsatisfying) {
                    return { };
                }

                auto x = gen();
                if(!pred(*x)) {
                    found_unsatisfying = true;
                    return { };
                }
                return std::move(x);
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( pred, false )

        template<typename Cont>
        Cont operator()(Cont cont) const
        {
            auto it = std::find_if_not(cont.begin(), cont.end(), pred);
            cont.erase(it, cont.end()); 
            return cont;
        }
    };

    /////////////////////////////////////////////////////////////////////
    struct take_last
    {
        const size_t cap;

        template<typename Gen>
        auto operator()(seq<Gen> r) const -> std::vector<typename seq<Gen>::value_type>
        {
            // consume all elements, keep last `cap` in the queue
            std::vector<typename seq<Gen>::value_type> queue;

            queue.reserve(cap);

            size_t i = 0; // count of insertions

            for(auto x = r.get_gen()(); x; x = r.get_gen()()) {
                if(i < cap) {
                    // NB: can't call queue.resize() because that imposes
                    // default-constructible on value_type, so instead
                    // push_back in the beginning.
                    queue.push_back(std::move(*x));
                } else {
                    queue[i % cap] = std::move(*x);
                }
                ++i;
            }

            if(cap < i) {
                // put the contents in proper order.
                auto it = queue.begin();
                std::advance(it, i % cap); // oldest inserted element
                std::rotate(queue.begin(), it, queue.end());
            }

            return queue;
        }

        template<typename Iterable>
        Iterable operator()(Iterable&& inps) const
        {
            // Iterable may be a container or view
            static_assert(std::is_rvalue_reference<Iterable&&>::value, "");

            // in case cont is a view:
            impl::require_iterator_category_at_least<std::forward_iterator_tag>(inps);
            const auto size = std::distance(inps.begin(), inps.end());

            if(decltype(size)(cap) < size) {
                auto it = inps.begin();
                std::advance(it, size - cap);
                inps.erase(inps.begin(), it);
            }
            return std::move(inps);
        }

        template<typename Cont>
        Cont operator()(const Cont& cont) const
        {
            Cont ret{};

            if(cap < cont.size()) {
                auto it = cont.begin();
                std::advance(it, cont.size() - cap);
                ret.insert(ret.end(), it, cont.end());
            }
            return ret;
        }
    };

    /////////////////////////////////////////////////////////////////////////
    struct drop_last
    {
        const size_t n;

        template<typename InGen>
        struct gen
        {
            InGen gen;
           size_t cap;

            using value_type = typename InGen::value_type;
            using vec_t = std::vector<value_type>;

            vec_t queue;
           size_t i;
            
            auto operator()() -> maybe<value_type>
            {
                if(queue.capacity() < cap) {
                    queue.reserve(cap);
                }

                while(i < cap) {
                    auto x = gen();
                    if(!x) {
                        return { };
                    }
                    queue.push_back(std::move(*x));
                    ++i;
                }
                
                auto& dest = queue[i++ % cap];
                auto ret = std::move(dest);
                auto x = gen();
                if(!x) {
                    return { };
                } else {
                    dest = std::move(*x);
                    return { std::move(ret) };
                }
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( n, {}, 0 )

        template<typename Iterable>
        Iterable operator()(Iterable&& inps) const
        {
            // Iterable may be a container or view
            static_assert(std::is_rvalue_reference<Iterable&&>::value, "");

            // in case cont is a view:
            impl::require_iterator_category_at_least<std::forward_iterator_tag>(inps);
            const auto size = std::distance(inps.begin(), inps.end());

            if(decltype(size)(n) < size) {
                auto it = inps.begin();
                std::advance(it, size - n);
                inps.erase(it, inps.end());
            } else {
                inps.clear();
            }

            return std::move(inps);
        }


        template<typename Cont>
        Cont operator()(const Cont& cont) const
        {
            Cont ret{};

            if(n < cont.size()) {
                auto it = cont.begin();
                std::advance(it, cont.size() - n);
                ret.insert(ret.end(), cont.begin(), it);
            }

            return ret;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Pred>
    struct drop_while
    {
        Pred pred;

        template<typename InGen>
        struct gen
        {
            InGen gen;
             Pred pred;
             bool found_unsatisfying;

            using value_type = typename InGen::value_type;

            auto operator()() -> maybe<value_type>
            {
                auto x = gen();
                while(!found_unsatisfying && x && pred(*x)) {
                    x = gen();
                }

                found_unsatisfying = true;
                return x;
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( pred, false )

        template<typename Iterable>
        Iterable operator()(Iterable inps) const
        {
            auto it = std::find_if_not(inps.begin(), inps.end(), pred);
            inps.erase(inps.begin(), it);
            return inps;
        }
    };

    /////////////////////////////////////////////////////////////////////
    // used by take_first, drop_first
    struct call_count_lt
    {
        const size_t cap;
        mutable size_t num_calls;

        template<typename T>
        bool operator()(const T&) const
        {
            return num_calls++ < cap;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Pred>
    struct where
    {
        Pred pred; // NB: may be mutable lambda, e.g.
                   // where([i = 0]() mutable { return i++ < 10; }).
                   //
                   // In container-specific overloads will need to
                   // make a copy and use that to be const-compliant.

        /////////////////////////////////////////////////////////////////////////
        template<typename InGen>
        struct gen
        {
            InGen gen;
             Pred pred;

            using value_type = typename InGen::value_type;

            static_assert(std::is_same<decltype(pred(*gen())), bool>::value, "The return value of predicate must be convertible to bool.");
            // is_convertible<..., bool> would suffice, but if it's not bool there's
            // a good chance that the user code is inadvertently doing something
            // the programmer did not intend, so we'll be a little more stringent
            // to guard against this.

            auto operator()() -> maybe<value_type>
            {
                auto x = gen();
                while(x && !pred(*x)) {
                    x = gen();
                }

                return x;
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( pred )

        /////////////////////////////////////////////////////////////////////////

        // If cont is passed as const-reference, 
        // make another container, copying only the
        // elements satisfying the predicate.
        template<typename Cont>
        Cont operator()(const Cont& cont) const
        {
            Cont ret{};
            auto pred_copy = pred; // in case copy_if needs non-const access 
            std::copy_if(cont.begin(),
                         cont.end(), 
                         std::inserter(ret, ret.end()), 
                         pred_copy);
            return ret;
        }

        // Why the above overload won't bind to non-const-reference args,
        // necessitating this overload?
        template<typename Cont>
        Cont operator()(Cont& cont) const
        {
            const Cont& const_cont = cont;
            return this->operator()(const_cont);
        }

        // If cont is passed as rvalue-reference,
        // erase elements not satisfying predicate.
        // (this also supports the case of container
        // with move-only value-type).
        template<typename Cont>
        Cont operator()(Cont&& cont) const //-> typename std::enable_if<std::is_rvalue_reference<Cont&&>::value, Cont>::type
        {
            static_assert(std::is_rvalue_reference<Cont&&>::value, "");

            x_EraseFrom(cont, impl::resolve_overload{}); // SFINAE-dispatch to erase-remove or iterate-erase overload.
            return std::move(cont); // must move, because Cont may contain move-only elements
                                    // (e.g. won't compile otherwise with Cont=std::vector<std::unique_ptr<int>>)
        }

    private:
        template<typename Cont>
        void x_EraseRemove(Cont& cont) const
        {
            auto pred_copy = pred;
            cont.erase(
                std::remove_if(
                    cont.begin(), cont.end(),
                    [&pred_copy](const typename Cont::value_type& x)
                    {
                        return !pred_copy(x);
                    }),
                cont.end());

           // Using std::not1(pred) instead of the lambda above won't compile:
           // error: no type named 'argument_type'...
        }


        // High-priority overload for containers where can call remove_if.
        template<typename Cont>
        auto x_EraseFrom(Cont& cont, pr_high) const -> decltype(void(cont.front()))
        {                                                         // ^^^^^^^^^^ discussion below
            // This overload must be made unpalatable to SFINAE unless 
            // std::remove_if is viable for Cont, e.g. feature-checking
            // as follows:
            //
            // A) Can call std::remove:
            //     -> decltype(void( std::remove(cont.begin(), cont.end(), *cont.begin()) ))
            //
            // B) Or simplifying, iterator's reference_type is non-const (assignable),
            //     -> decltype(void( *cont.begin() = std::move(*cont.begin()) ))
            //  or -> decltype(std::swap(*cont.begin(), *cont.begin()))
            //
            // For a std::map, even though map's value_type is not move-assignable, 
            // (due to key being const, the lines below should not and would not compile, 
            // and yet SFINAE still considers this overload viable (??)
            //
            //     std::remove(cont.begin(), cont.end(), *cont.begin());
            //     *cont.begin() = std::move(*cont.begin());
            //
            // I'm not sure why SFINAE fails to reject it in the decltype,
            // resulting in ambiguous resolution of x_EraseFrom.
            //
            // Anyway, that's why we're instead feature-testing for SequenceContainer 
            // based the presence of cont.front()
            // https://en.cppreference.com/w/cpp/named_req/SequenceContainer
            //
            // Update: This is no longer an issue with newer compilers, so this must have been
            // a compiler bug. However, Leaving the cont.front() check in place for now
            // to support older compilers.

            x_EraseRemove(cont); 
        }

        // for a view I can't tell which idiom to use in SFINAE 
        // (e.g. is it a view into a map or into a vec?)
        // So will specialize for view and require random_access_iterator,
        // so we can be sure erase-remove is viable
        template<typename Iterator>
        void x_EraseFrom(view<Iterator>& v, pr_high) const
        {
            impl::require_iterator_category_at_least<std::random_access_iterator_tag>(v);
            x_EraseRemove(v);
        }

        // Low-priority overload where remove_if is not viable, but
        // the container has equal_range method (e.g. set and associative
        // containers) so it is reasonable to expect that iterate-erase
        // idiom is applicable.
        template<typename Cont>
        auto x_EraseFrom(Cont& cont, pr_low) const
          -> decltype(void(
                cont.equal_range(
                    std::declval<typename Cont::key_type const&>())))
        {
            auto pred_copy = pred;
            for(auto it = cont.begin(), it_end =cont.end(); 
                     it != it_end; 
                     it = pred_copy(*it) ? std::next(it)
                                         : cont.erase(it))
            {
                ;
            }
        }

        template<typename NotACont>
        static void x_EraseFrom(NotACont&, pr_lowest)
        {
            static_assert(sizeof(NotACont) == 0, "The argument to fn::impl::where is expected to be either a seq<...> or a sequence container or an associative container having equal_range method.");
            TheTypeInQuestionIs<NotACont>{};
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename SortedRange, typename F>
    struct in_sorted_by
    {
         const SortedRange& r;
                 const bool is_subtract; // subtract or intersect r
        const impl::comp<F> comp;

        bool operator()(const typename SortedRange::value_type& x) const
        {
            return is_subtract ^ std::binary_search(r.begin(), r.end(), x, comp);
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct where_max_by
    {
        const F   key_fn;
        const int use_max; // 1 for max, -1 for use-min

        /////////////////////////////////////////////////////////////////
        // Note: rather than returning a seq, as with where() or transform(),
        // we're returning a vector with accumulated results (we had to accumulate
        // the results somewhere, and there's no point in wrapping back in seq.
        template<typename InGen,
                 typename Ret = typename std::vector<typename seq<InGen>::value_type> >
        auto operator()(seq<InGen> in) const -> Ret
        {
            Ret ret{};

            auto& gen = in.get_gen();

            for(auto x = gen(); x; x = gen()) {

                const auto which = ret.empty() ? 0 
                                 : impl::compare(key_fn(ret.front()), 
                                                 key_fn(*x)) * use_max;
                if(which < 0) {
                    ret.clear();
                    ret.push_back(std::move(*x));

                } else if(which > 0) {
                    ;
                } else {
                    ret.push_back(std::move(*x));
                }
            }

            if(ret.capacity() >= ret.size() * 2) {
                ret.shrink_to_fit();
            }

            return ret;
        }

        /////////////////////////////////////////////////////////////////
        template<typename Cont,
                 typename Ret = std::vector<typename Cont::value_type> >
        Ret operator()(Cont cont) const
        {
            // TODO: Take Cont as forwarding reference instead of by value
            // such that if it is passed by lvalue-reference we don't need to
            // copy all elements; will only copy the maximal ones.
            // (copy or move value from the iterator depending on whether 
            // cont is lvalue or rvalue-reference?)

            // NB[2]: if we're taking lvalue of key_fn(element),
            // be mindful that key may contain references to keyed object.
            // (hopefully const, but we can't even assume that).
            // (e.g. make_tuple(....) unwraps reference-wrappers and returns
            // a tuple that may contain references.
            //
            // Therefore:
            // 1) we can't reassign keys, e.g. best_key = max(best_key, current_key),
            //    as this will assign the internal reference (if non-const)
            //    or will not compile (if reference is const).
            //
            // 2) The value of key may become invalidated if referenced
            //    element is assigned or moved from (explicitly, or under the hood, 
            //    e.g. while reallocating when a std::vector is resized in push_back,
            //    or when the element is moved to a different position by erase-remove
            //    algorithm.
           
            // NB: we could reuse the same logic for both input and Cont use-cases,
            // but having multi-pass capability with Cont allows us to do less
            // memory churn (we only need to move elements-of-interest into ret).

            Ret ret{};

            if(cont.empty())
                return ret;
            
            auto first_best_it = cont.begin();
            auto last_best_it  = cont.begin();
            size_t n = 1; // count of max-elements

            {
                auto it = cont.begin();
                for(++it; it != cont.end(); ++it) {
                    const int which = impl::compare(key_fn(*it), 
                                                    key_fn(*first_best_it)) * use_max;
                    if(which < 0) {
                        ; // not-best
                    } else if(which > 0) {
                        // new-best
                        first_best_it = last_best_it = it;
                        n = 1;
                    } else {
                        last_best_it = it;
                        n++;
                    }
                }
            }

            ret.reserve(n);
            ret.push_back(std::move(*first_best_it));
            ++first_best_it;
            ++last_best_it; // convert to end

            for(auto it = first_best_it; it != last_best_it; ++it) 
                if(impl::compare(key_fn(*it), 
                                 key_fn(ret.back())) * use_max >= 0)
            {
                ret.push_back(std::move(*it));
            }

            return ret;
            
#if 0 // An example of what NOT to do:
            auto best_key = key_fn(*cont.begin());
            for(const auto& e : cont) {
                auto key = key_fn(e);
                if(impl::compare(best_key, key)*use_max > 0) {
                    best_key = std::move(key);  // BUG: modifies referenced object via
                                                // internal non-const references in key.
                                                // (or will not compile if references are const)
            }

            return fn::where(
                    [&, this](const typename Ret::value_type& x)
                    {
                        return impl::compare(key_fn(x), best_key)*use_max >= 0;
                    })(std::forward<Cont>(cont));
            // BUG: best_key may contain a reference, which is invalidated mid-processing
            // when best-element is moved inside erase-remove loop in fn::where.
#endif
        }
    };


    /////////////////////////////////////////////////////////////////////
    struct reverse
    {
        template<typename Gen>
        auto operator()(seq<Gen> r) const -> std::vector<typename seq<Gen>::value_type>
        {
            auto vec = to_vector{}(std::move(r));
            std::reverse(vec.begin(), vec.end());
            return vec;
        }

        template<typename Iterable>
        struct reversed
        {
            Iterable src;
            using value_type     = typename Iterable::value_type;
            using iterator       = typename Iterable::reverse_iterator;
            using const_iterator = typename Iterable::const_reverse_iterator;

            iterator begin()
            {
                return src.rbegin();
            }

            iterator end()
            {
                return src.rend();
            }

            const_iterator begin() const
            {
                return src.rbegin();
            }

            const_iterator end() const
            {
                return src.rend();
            }

            const_iterator cbegin() const
            {
                return src.crbegin();
            }

            const_iterator cend() const
            {
                return src.crend();
            }
        };

        template<typename Iterable>
        reversed<Iterable> operator()(Iterable src) const
        {
            return reversed<Iterable>{ std::move(src) };
        }

        template<typename Iterator>
        view<std::reverse_iterator<Iterator>> operator()(view<Iterator> v) const
        {
            impl::require_iterator_category_at_least<std::bidirectional_iterator_tag>(v);

            using rev_it_t = std::reverse_iterator<Iterator>;
            return { rev_it_t{ v.end()   }, 
                     rev_it_t{ v.begin() } };
        }
    };

    // NB: sort and reverse have identical signatures for overloads

    /////////////////////////////////////////////////////////////////////
    // I made a decision to deviate from STL naming convention,
    // where std::sort is unstable, and made fn::sort/sort_by a stable-sort.
    // 
    // I think STL should have followed the principle of least astonishment and made
    // std::sort a stable-sort, and have an additonal std::unstable_sort version.
    // (STL is known for its poor names that tend to surprise novice programmers).
    // That is, by default sort should do "the most right" thing,
    // and have an differently named (e.g. "unstable_sort") optimizing
    // version that can make additional assumptions about the inputs.
    //
    // In real-life sort_by/group_all_by/unique_all_by use-cases we don't 
    // simply sort ints or strings where unstable-sort would do; rather,
    // we sort some objects by some subset of properties, where the sort-key only
    // partially differentiates between the objects, and a two objects
    // that are equivalent according to the sort key don't necessarily
    // compare equal, and so we shouldn't be swapping their relative order.
    struct stable_sort_tag {};
    struct unstable_sort_tag {};
    template<typename F, typename SortTag = stable_sort_tag>
    struct sort_by
    {
        const F key_fn;

        template<typename Gen>
        auto operator()(seq<Gen> r) const -> std::vector<typename seq<Gen>::value_type>
        {
            return this->operator()(to_vector{}(std::move(r)));
        }

        template<typename Iterable>
        Iterable operator()(Iterable src) const
        {
            impl::require_iterator_category_at_least<std::random_access_iterator_tag>(src);

            // this will fire if Iterable is std::map, where value_type is std::pair<const Key, Value>,
            // which is not move-assignable because of constness.
            static_assert(std::is_move_assignable<typename Iterable::value_type>::value, "value_type must be move-assignable.");

            s_sort( src, 
                    [this](const typename Iterable::value_type& x, 
                           const typename Iterable::value_type& y)
                    {
                        return lt{}(key_fn(x), key_fn(y));
                    }
                    , SortTag{});

            return src;
        }

    private:
        template<typename Iterable, typename Comp>
        static void s_sort(Iterable& src, Comp comp, stable_sort_tag)
        {
            std::stable_sort(src.begin(), src.end(), std::move(comp)); // [compilation-error-hint]: expecting sortable container; try fn::to_vector() first.
        }

        template<typename Iterable, typename Comp>
        static void s_sort(Iterable& src, Comp comp, unstable_sort_tag)
        {
            std::sort(src.begin(), src.end(), std::move(comp)); // [compilation-error-hint]: expecting sortable container; try fn::to_vector() first.
        }
    };

    /////////////////////////////////////////////////////////////////////////

    // NB: initially thought of having stable_sort_by and unstable_sort_by versions,
    // and for unstable_sort_by/Cont use std::sort, and for unstable_sort_by/seq<..>
    // seq use lazy heap sort. The unstable-ness, however, is different between the two. 
    // So instead decided to go with lazy_sort_by for both Cont and seq<>

    // Initially dump all elements and heapify in O(n);
    // lazily yield elements with pop_heap in O(log(n))
    //
    // TODO: can we make this stable? E.g. falling-back on pointer comparison (probably not)
    // or keeping a vector of std::pair<decltype(gen()), size_t> where ::second is the 
    // original ordinal.
    template<typename F>
    struct lazy_sort_by
    {
        const F key_fn;

        template<typename InGen>
        struct gen
        {
              InGen gen;
            const F key_fn;

            using value_type = typename InGen::value_type;
            using vec_t = std::vector<value_type>;

            static_assert(std::is_move_assignable<value_type>::value, "value_type must be move-assignable.");

            vec_t heap;
             bool heapified;

            auto operator()() -> maybe<value_type>
            {
                auto op_gt = [this](const value_type& x, 
                                    const value_type& y)
                {
                    return lt{}(key_fn(y), key_fn(x));
                };

                if(!heapified) {
                    assert(heap.empty());
                    heapified = true;

                    // TODO: if gen is to_gen wrapper, move elements
                    // directly from the underlying Iterable.
                    for(auto x = gen(); x; x = gen()) {
                        heap.push_back(std::move(*x));
                    }

                    std::make_heap(heap.begin(), heap.end(), op_gt);
                }

                if(heap.empty()) {
                    return { };
                }

                std::pop_heap(heap.begin(), heap.end(), op_gt);
                auto ret = std::move(heap.back());
                heap.pop_back();
                return { std::move(ret) };
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ(  key_fn, {}, false )
        RANGELESS_FN_OVERLOAD_FOR_CONT( key_fn, {}, false )
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct take_top_n_by
    {
        const F      key_fn;
        const size_t cap;

        template<typename Iterable>
        auto operator()(Iterable src) const -> std::vector<typename Iterable::value_type>
        {
            // TODO: if Iterable is a vector, do partial-sort, decreasing, take_first(n), and reverse.
            //
            // Take by forwarding-reference here instead of by-value?
            // (Same question as in where_max_by)

            using value_type = typename Iterable::value_type;

            // this will fire if Iterable is std::map, where value_type is std::pair<const Key, Value>,
            // which is not move-assignable because of constness.
            static_assert(std::is_move_assignable<value_type>::value, "value_type must be move-assignable.");

            auto op_gt = [this](const value_type& x, 
                                const value_type& y)
            {
                return lt{}(key_fn(y), key_fn(x));
            };

            // NB: can't use priority_queue, because it provides 
            // const-only exposition of elements, so we can't use
            // it with move-only types.

            std::vector<value_type> heap{};  // min-heap, (min at front)
            heap.reserve(cap);

            if(cap > 0)
                for(auto&& x : src)
            {
                const bool insert = 
                    heap.size() < cap 
                 || [&]
                    {
                        const auto& key_x = key_fn(x);   // NB: temporary lifetime extension
                        const auto& key_h = key_fn(heap.front());

                        // key_fn is expected to be "light", but in case it's not negligibly light
                        // we factor-out evaluation of key_fn here, because we can.

                        return lt{}(key_x, key_h) ? false  // worse than current-min
                             : lt{}(key_h, key_x) ? true   // better than current-min
                             :                      false; // equiv to current-min (we're at capacity, so keeping current min)
                    }();

                if(!insert) {
                    continue;
                }

                if(heap.size() >= cap) {
                    std::pop_heap(heap.begin(), heap.end(), op_gt);
                    heap.pop_back();
                }

                heap.push_back(std::move(x));
                std::push_heap(heap.begin(), heap.end(), op_gt);
            }

            std::sort_heap(heap.begin(), heap.end(), op_gt);
            std::reverse(heap.begin(), heap.end());
            
            return heap;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F, typename BinaryPred = impl::eq>
    struct group_adjacent_by
    {
        // We parametrize to both key_fn and pred2 to reuse
        // this for both
        // group_adjacent_by: key_fn:user-provided,      pred2=impl::eq
        // group_adjacent_if: key_fn=impl::by::identity, pred2:user-provided.
        
                 const F key_fn;
        const BinaryPred pred2;

        /////////////////////////////////////////////////////////////////////
        // A simple version that accumulates equivalent-group in a vector, 
        // and yield vector_t. This makes it easy to deal-with (no seq-of
        // seqs), but it needs to allocate a return std::vector per-group.
        // (Edit: unless the use-case supports recycling (see below))
        //
        template<typename InGen>
        struct gen
        {
                          InGen gen;
                        const F key_fn;
               const BinaryPred pred2;

            using inp_t      = typename InGen::value_type;

            using value_type = typename std::conditional<
                                    std::is_same<inp_t, char>::value, 
                                        std::string, 
                                        std::vector<inp_t> >::type;

            static_assert(std::is_move_assignable<value_type>::value, "value_type must be move-assignable.");


            value_type next;
            value_type garbage;

            // instead of allocating a new vector, the caller, prior to calling
            // the operator(), may choose to disown existing vector so we can
            // reuse its internal storage

            void recycle(value_type& grbg)
            {
                garbage = std::move(grbg);
            }

            auto operator()() -> maybe<value_type>
            {
                // NB: number of calls to key_fn shall be max(0, 2*(n-1))
                // (as required by fn::chunker)

                auto curr = std::move(next);
                next = std::move(garbage);
                next.clear(); // clearing does not deallocate internal storage

                for(auto x = gen(); x; x = gen()) {
                    if(curr.empty() || pred2(key_fn(curr.back()), key_fn(*x))) {
                        curr.push_back(std::move(*x));
                    } else {
                        next.push_back(std::move(*x));
                        break;
                    }
                }

                if(curr.empty()) {
                    return { };
                }
                
                return std::move(curr);
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( key_fn, pred2, {}, {} )

        // view may be an InputRange, so treat as seq.
        RANGELESS_FN_OVERLOAD_FOR_VIEW( key_fn, pred2, {}, {} )

        /////////////////////////////////////////////////////////////////////
        template<typename Cont>
        auto operator()(Cont cont) const -> std::vector<Cont>
        {
            // NB: number of calls to key_fn shall be max(0, 2*(n-1))
            // (see chunker)

            std::vector<Cont> ret;

            auto it = cont.begin();
            const auto it_end = cont.end();

            if(it == it_end) {
                return ret;
            }

            ret.push_back(Cont{});
            ret.back().insert(ret.back().end(), std::move(*it)); // [compilation-error-hint]: Expecting Cont's value-type to be end-insertable container.
            ++it;

            for(; it != it_end; ++it) {
                if(!pred2(key_fn(*ret.back().crbegin()), // NB: last!
                          key_fn(*it)))
                {
                    ret.push_back(Cont{});
                }
                auto& dest = ret.back();
                dest.insert(dest.end(), std::move(*it));
            }

            return ret;
        }
    };


    /////////////////////////////////////////////////////////////////////
    template<typename F, typename BinaryPred = impl::eq>
    struct group_adjacent_as_subseqs_by
    {
                 const F key_fn;
        const BinaryPred pred2;

        template<typename InGen>
        struct gen
        {
            // subgen for subseq - yield elements of the same group
            struct subgen
            {
                using value_type = typename InGen::value_type;
                gen* parent;

                maybe<value_type> operator()()
                {
                    assert(parent);
                    return parent->next();
                }
            };

            /////////////////////////////////////////////////////////////////

            using element_type = typename InGen::value_type;
            using value_type = seq<subgen>;

                          InGen in_gen;
                        const F key_fn;
               const BinaryPred pred2;   // returns true iff two elemens belong to same group
            maybe<element_type> current; // current element of the current group
                           bool reached_subend; 

            /////////////////////////////////////////////////////////////////

            maybe<element_type> next() // called from subgen::operator()()
            {
                if(!current || reached_subend) {
                    reached_subend = true;
                    return {};
                }

                auto next = in_gen();
                reached_subend = !next || !pred2(key_fn(*current), key_fn(*next));
                std::swap(current, next);
                return std::move(next);
            }
            
            maybe<value_type> operator()() // return seq for next group
            {
                if(!current) { // starting initial group
                    current = in_gen();
                } else {

                    // NB: the user-code may have accessed the group only
                    // partially (or not at all), in which case we must skip
                    // over the rest elements in the group until we reach 
                    // the next one.
                    while(!reached_subend) {
                        next();
                    }
                }

                if(!current) { // reached final end
                    return {};
                }

                reached_subend = false;
                return { subgen{ this } };
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ(  key_fn, pred2, {}, false )
        RANGELESS_FN_OVERLOAD_FOR_VIEW( key_fn, pred2, {}, false )
        RANGELESS_FN_OVERLOAD_FOR_CONT( key_fn, pred2, {}, false )
    };


    /////////////////////////////////////////////////////////////////////
    // used for in_groups_of(n)
    struct chunker
    {
        const size_t chunk_size;
        mutable size_t n_calls;

        template<typename T>
        size_t operator()(const T&) const
        {
            // chunker is called with group_adjacent_by.
            // If we had one call to key_fn per element, 
            // we could chunk in groups of 10 like
            // elems % fn::group_adjacent_by([n = 0UL](const auto){ return n++/10; });
            //
            // in group_adjacent_by we call the key_fn 2*(n-1) times.
            return ++n_calls/2/chunk_size;
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct group_all_by
    {
    private:
        using group_adjacent_by_t = group_adjacent_by<F>;

    public:
        const F key_fn;

        // group_all_by: convert input to vector, sort by key_fn, 
        // and delegate to group_adjacent_by::gen
        //
        // (could instead delegate to the eager overload below,
        // since we need to do the sort anyway, but the difference
        // is that group_adjacent_by will recycle std::vector storage
        // while yielding groups one by one, rather than having to
        // allocate a separate std::vector per-group.

        template<typename Gen>  // Cont or seq<...>
        auto operator()(seq<Gen> range) const -> 
            seq<
                typename group_adjacent_by_t::template gen<
                    to_seq::gen<
                        std::vector<typename seq<Gen>::value_type>>>>
        {
            return 
                group_adjacent_by_t{ key_fn, {} }(
                    impl::to_seq{}(
                        sort_by<F>{ key_fn }(
                            impl::to_vector{}(
                                std::move(range)))));
        }

        /////////////////////////////////////////////////////////////////
        template<typename Cont>
        auto operator()(Cont cont) const
            //-> std::vector<std::vector<typename Cont::value_type>>
            //The above is almost correct, except if Cont is a map,
            //conversion to vector removes the key-constness, so we
            //need to do it via decltype
            -> decltype(
                    group_adjacent_by_t{ key_fn, {} }(
                        impl::to_vector{}(
                            std::move(cont))))
        {
#if 0 
            // An example of what not to do
            using key_t = remove_cvref_t<decltype(key_fn(*cont.begin()))>;
            std::map<key_t, Cont, lt_t> m;
            for(auto&& x : cont) {
                auto& dest = m[key_fn(x)];
                dest.insert(dest.end(), std::move(x)); 
                // BUG: the key in the map just became invalidated
                // after move(x), if it contained references to it.
            }
            cont.clear(); 

            std::vector<Cont> ret;
            ret.reserve(m.size());
            for(auto& kv : m) {
                ret.push_back(std::move(kv.second));
            }
            return ret;

            // Can't also implement is as collect elems into
            // std::set<Cont, decltype(opless_by_keyfn_on_element)>,
            // because if value_type is move-only, we won't be able
            // to move-out the elements from the set since 
            // set provides only const access to the elements.
#else
            return group_adjacent_by_t{ key_fn, {} }(
                impl::sort_by<F>{ key_fn }(
                    impl::to_vector{}(
                        std::move(cont))));
#endif
        }
    };



    /////////////////////////////////////////////////////////////////////
    // Flatten a range-of-ranges.
    // Inverse of group_all_by or group_adjacent_by(original elements are in key-order)
    //
    // This is the bind operator for the List monad.
    //
    // What do we name this? 
    //
    //    Haskell: concat
    //     Elixir: concat
    //        Elm: concat
    //     O'Caml: concat, flatten
    //      Scala: flatten
    //    clojure: flatten
    //     python: chain
    //          D: chain
    //         F#: collect
    // javascript: flat
    //   range-v3: join
    //       LINQ: SelectMany
    //
    struct concat
    {
        template<typename InGen>
        struct gen
        {
            InGen gen;

            using gen_value_t = typename InGen::value_type;
            using group_t     = maybe<gen_value_t>;
                               // via maybe, because might not be default-constructible;
                               // or could be a seq<...>, holding a closure under the hood,
                               // rendering the seq not move-assigneable.

            using iterator    = typename gen_value_t::iterator;
            using value_type  = typename gen_value_t::value_type;

             group_t current_group;
            iterator it;

#if 0       // We don't really need this

            // -Weffc++ recommends explicit assignment operator because
            // we have pointer members (presumably current_group), so we
            // explicitly make this move-only.
            gen& operator=(gen&&) = default;
                       gen(gen&&) = default;
            
            gen& operator=(const gen&) = delete;
                       gen(const gen&) = delete;
#endif

            auto operator()() -> maybe<value_type>
            {
                // get next group if !started or reached end-of-current
                while(!current_group || it == (*current_group).end()) {
                    current_group = gen();
                    
                    if(!current_group) {
                        return { };
                    }

                    it = (*current_group).begin();
                }

                //postfix++ may not be available
                value_type ret = std::move(*it);
                ++it;
                return { std::move(ret) };
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( {}, typename gen<InGen>::iterator() )
                                    //^^ simply {} does not compile with GCC-4.9.3

        /////////////////////////////////////////////////////////////////
        
        template<typename Iterable,
                 typename Ret = typename Iterable::value_type>
        Ret operator()(Iterable src) const
        {
            // TODO: return a vector instead? What if cont is a set or a map?

            Ret ret{};
            ret.clear(); // [compilation-error-hint]: Expecting Iterable::value_type to be a container.

            for(auto&& v : src) {
                ret.insert(ret.end(),
                           std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
            }

            return ret;
        }

        // The case of vector-of-seq can't be handled by the above overload,
        // because seq can't be end-inserted into.
        // So we'll wrap container-of-seq into seq, and treat as seq-of-seq.
        template<typename Gen>
        auto operator()(std::vector<seq<Gen>> vec_of_seqs) const
          -> seq<gen< to_seq::gen< std::vector<seq<Gen>> > >>
        {                       //^^^^^^^^^^^^^^^^^^^^^       input vec
                     //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^     wrapped as seq
          //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  composed with this
          // TODO: needs a test.

            return { { { std::move(vec_of_seqs) } } };
        }
    };


    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct unique_adjacent_by
    {
        const F key_fn;

        template<typename InGen>
        struct gen
        {
              InGen gen;
            const F key_fn;

            using value_type = typename InGen::value_type;

            using state_t = maybe<value_type>;
            // wrapped into maybe, because might not be default-constructible

            state_t next;

            auto operator()() -> maybe<value_type>
            {
                auto curr = std::move(next);
                next.reset();

                for(auto x = gen(); x; x = gen()) {
                    if(!curr) {
                        curr = std::move(x);
                    } else if( impl::eq{}(key_fn(*curr), key_fn(*x))) {
                        continue; // skip equivalent elements
                    } else {
                        next = std::move(x);
                        break;
                    }
                }

                return curr;
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( key_fn, {} )

        template<typename Iterable>
        Iterable operator()(Iterable inps) const
        {
            impl::require_iterator_category_at_least<std::forward_iterator_tag>(inps);

            inps.erase(
                std::unique(
                    inps.begin(), inps.end(),
                    [this](const typename Iterable::value_type& x, 
                           const typename Iterable::value_type& y)
                    {
                        return impl::eq{}(key_fn(x), key_fn(y));
                    }),
                inps.end());

            return inps;
        }
    };



    /////////////////////////////////////////////////////////////////////
    template<typename F>
    struct unique_all_by
    {
        const F key_fn;

        template<typename InGen>
        struct gen
        {
              InGen gen;
            const F key_fn; // lifetime of returned key shall be independent of arg.

            using value_type = typename InGen::value_type;

            using key_t = typename std::decay<decltype(key_fn(*gen()))>::type; 
            // key_fn normally yields a const-reference, but we'll be storing 
            // keys in a map, so need to decay the type (remove_cvref would do).

            static_assert(std::is_default_constructible<key_t>::value, "The type returned by key_fn in unique_all_by must be default-constructible and have the lifetime independent of arg.");
            // In case key_fn returns a reference-wrapper or a tie-tuple containing
            // a reference, these will become invalidated as keys in the map
            // when the referenced object goes out of scope, so we guard against
            // these by requiring default-constructible on key_t.

            using seen_t = std::map<key_t, bool>; 
            // might as well have used std::set, but to #include fewer things will
            // repurpose std::map that's already included for other things.

            seen_t seen;

            auto operator()() -> maybe<value_type>
            {
                auto x = gen();

                if(!x) {
                    return { };
                }
                auto k = key_fn(*x);

                while(!seen.emplace(std::move(k), true).second) {
                    x = gen();
                    if(!x) {
                        break;
                    }
                    k = key_fn(*x);
                }
                return x;
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ( key_fn, {} )


        template<typename Iterable>
        auto operator()(Iterable src) const
          -> decltype( // see the corresponding overload in group_all_by
                  unique_adjacent_by<F>{ key_fn }(
                      to_vector{}(
                          std::move(src))))
        {
            return unique_adjacent_by<F>{ key_fn }(
                sort_by<F>{ key_fn }(
                    to_vector{}(
                        std::move(src))));
        }
    };


    /////////////////////////////////////////////////////////////////////
    // Concat a pair of (possibly heterogeous) `Iterables`.
    template<typename Iterable2>
    struct append
    {
        Iterable2 src2;

        template<typename Iterable1>
        struct gen
        {
                               Iterable1 inps1;
                               Iterable2 inps2;
            typename Iterable1::iterator it1;
            typename Iterable2::iterator it2;
                                     int which; // 0: not-started; 
                                                // 1: in range1
                                                // 2: in range2
            using value_type = 
                typename std::common_type<
                    typename Iterable1::value_type,
                    typename Iterable2::value_type
                                         >::type;

            auto operator()() -> maybe<value_type>
            {
                return which == 1 && it1 != inps1.end() ? std::move(*it1++)
                     : which == 2 && it2 != inps2.end() ? std::move(*it2++)
                     : which == 0 ? (which = 1, it1 = inps1.begin(), (*this)())
                     : which == 1 ? (which = 2, it2 = inps2.begin(), (*this)())
                     :              maybe<value_type>{ };
            }
        };

        template<typename Iterable1>
        auto operator()(Iterable1 src1) && -> seq<gen<Iterable1>> // rvalue-specific because src2 is moved-from
        {
            return { { std::move(src1), std::move(src2), {}, {}, 0 } };
        }
    };

    /////////////////////////////////////////////////////////////////////
    template<typename Iterable2, typename BinaryFn>
    struct zip_with
    {
        Iterable2 src2;
        BinaryFn  fn;

        template<typename Iterable1>
        struct gen
        {
                                BinaryFn fn;
                               Iterable1 src1;
                               Iterable2 src2;
            typename Iterable1::iterator it1;
            typename Iterable2::iterator it2;
                                    bool started;

            using value_type = decltype(fn(std::move(*it1),
                                           std::move(*it2)));

            auto operator()() -> maybe<value_type>
            {
                if(!started) {
                    it1 = src1.begin();
                    it2 = src2.begin();
                    started = true;
                }

                if(it1 == src1.end() || it2 == src2.end()) {
                    return { };
                }

                auto ret = fn(std::move(*it1), std::move(*it2));
                ++it1;
                ++it2;
                return { std::move(ret) };
            }
        };

        template<typename Iterable1>
        auto operator()(Iterable1 src1) && -> seq<gen<Iterable1>> // rvalue-specific because src2 is moved-from
        {
            return { { std::move(fn), std::move(src1), std::move(src2), {}, {}, false } };
        }
    };

    /////////////////////////////////////////////////////////////////////////
    template<typename BinaryFn>
    struct zip_adjacent
    {    
        BinaryFn fn;

        template<typename Iterable>
        struct gen
        {    
                              BinaryFn fn;
                              Iterable src; 
           typename Iterable::iterator it;
                                  bool started;

            using value_type = decltype(fn(*it, *it));

            auto operator()() -> maybe<value_type>
            {    
                if(!started) {
                    it = src.begin();
                    started = true;
                }    

                auto prev_it = it == src.end() ? it : it++;

                if(it != src.end()) {
                    return { fn(*prev_it, *it) };
                } else {
                    return { }; 
                }    
            }    
        };   

        template<typename Iterable>
        auto operator()(Iterable src) && -> seq<gen<Iterable>>
        {    
            impl::require_iterator_category_at_least<std::forward_iterator_tag>(src);
            return { { std::move(fn), std::move(src), {}, false } }; 
        }    
    };

    /////////////////////////////////////////////////////////////////////////
    template<typename Iterable2, typename BinaryFn>
    struct cartesian_product_with
    {
        Iterable2 src2;
        BinaryFn fn;

        template<typename Iterable1>
        struct gen
        {
                                BinaryFn fn;
                               Iterable1 src1;
                               Iterable2 src2;
            typename Iterable1::iterator it1;
            typename Iterable2::iterator it2;
                                    bool started;

            // Checking against input-range semantics.
            // Checking against moving iterators (yielding rvalue-references)
            // because if fn takes a param by value and the iterator yields 
            // it will be moved-from.
            static_assert(!std::is_rvalue_reference<decltype(*--it1)>::value, "For cartesian-product expecting upstream iterable to be a bidirectional range yielding lvalue-references or values");
            static_assert(!std::is_rvalue_reference<decltype(*--it2)>::value, "For cartesian-product expecting parameter-iterable to be a bidirectional range yielding lvalue-references or values");

            using value_type = decltype(fn(*it1, *it2));

            auto operator()() -> maybe<value_type>
            {
                if(!started) {
                    it1 = src1.begin();
                    it2 = src2.begin();
                    started = true;
                    if(it1 == src1.end() || it2 == src2.end()) {
                        return { };
                    }
                }

                while(it2 == src2.end()) {
                    it2 = src2.begin();

                    ++it1;
                    if(it1 == src1.end()) {
                        return { };
                    }
                }

                auto ret = fn(*it1, *it2);
                ++it2;
                return { std::move(ret) };
            }
        };

        template<typename Iterable1>
        auto operator()(Iterable1 src1) && -> seq<gen<Iterable1>> // rvalue-specific because range2 is moved-from
        {
            return { { std::move(fn), std::move(src1), std::move(src2), {}, {}, false } };
        }
    };

    /////////////////////////////////////////////////////////////////////////

    struct memoizer_detail
    {
        template<class Ret, class Arg>
        struct lambda_traits_detail
        {    
            using ret = Ret; 
            using arg = Arg;
        };   

        template<class L>
        struct lambda_traits
             : lambda_traits<decltype(&L::operator())>
        {};

        // NB: not providing non-const version (must be pure)
        template<class Ret, class Class, class Arg>
        struct lambda_traits<Ret(Class::*)(Arg) const>
             : lambda_traits_detail<Ret, Arg>
        {};
    };

    template<typename F>
    struct memoizer
    {
        using traits = impl::memoizer_detail::lambda_traits<F>;
        using    Arg = typename std::remove_reference<typename traits::arg>::type;
        using    Ret = typename traits::ret; // NB: can be const and/or reference
        using  Cache = std::map<Arg, Ret>;

                    F fn;
        mutable Cache m; // mutable, because operator() must be const
                         // e.g. we may be memoizing some key_fn, and 
                         // those are expected to be const

        const Ret& operator()(const Arg& arg) const
        {
            auto it = m.find(arg);
            return it != m.end() ? it->second
                                 : m.emplace(arg, fn(arg)).first->second;
        }
    };

    /////////////////////////////////////////////////////////////////////////
    template<typename F>
    struct scope_guard
    {
           F fn;
        bool dismissed;

        void dismiss()
        {
            dismissed = true;
        }

        ~scope_guard() noexcept(noexcept(fn()))
        {
            if(!dismissed)
                fn(); // NB: it's up to m_fn to decide whether to catch exceptions.
        }
    };


}   // namespace impl

    /////////////////////////////////////////////////////////////////////////

    /// @defgroup to_vec to_vector/to_seq
    /// @{

    /// @brief Move elements of an `Iterable` to std::vector.
    inline impl::to_vector to_vector()
    {
        return {};
    }

    /// @brief Wrap an `Iterable`, taken by value, as `seq` yielding elements by-move.
    ///
    /// This is a dual of `to_vector`. It can be used to adapt your own `InputRange` as
    /// `seq`, but it can also be used to wrap a container to force lazy evaluation
    /// e.g. `std::move(container) % fn::to_seq() % fn::group_adjacent_by(key_fn) % fn::take_first()`
    inline impl::to_seq to_seq()
    {
        return {};
    }

    /// e.g. `auto set_of_ints = fn::seq(...) % ... % fn::to(std::set<int>{})`;
    template<typename Container>
    impl::to<Container> to(Container dest)
    {
        return { std::move(dest) };
    }

    /// @brief return map: value_type -> size_t
    inline impl::counts counts()
    {
        return {};
    }


    /// @}
    /// @defgroup transform Transform and Adapt
    ///
    /// @{

    /// @brief Create a custom processing-stage function-object.
    ///
    /// This is somewhat similar to `fn::transform`, except the correspondence between
    /// inputs and outputs is not necessarily 1:1, and instead of taking a single element to transform
    /// we take a nullary generator callable `gen` and use it to fetch however many input elements
    /// we need to generate the next output element.
    /// 
    /// NB: If `bool(gen)==false`, the next invocation of gen() shall throw `fn::end_seq::exception` signaling end-of-inputs.
    /*!
    @code
        auto my_transform = [](auto fn)
        {
            return fn::adapt([fn = std::move(fn)](auto gen)
            {
                return fn(gen());
            });
        };

        auto my_where = [](auto pred)
        {
            return fn::adapt([pred = std::move(pred)](auto gen)
            {
                auto x = gen();
                while(!pred(x)) {
                    x = gen();
                }
                return x;
            });
        };

        auto my_take_while = [](auto pred)
        {
            return fn::adapt([pred = std::move(pred)](auto gen)
            {
                auto x = gen();
                return pred(x) ? std::move(x) : fn::end_seq();
            });
        };

        auto my_intersperse = [](auto delim)
        {
#if 1
            return [delim = std::move(delim)](auto inputs)
            {
                return fn::seq([  delim, 
                                 inputs = std::move(inputs), 
                                     it = inputs.end(), 
                                started = false, 
                                   flag = false]() mutable
                {
                    if(!started) {
                        started = true;
                        it = inputs.begin();
                    }
                    return it == inputs.end() ? fn::end_seq()
                         :     (flag = !flag) ? std::move(*it++)
                         :                      delim;
                });
            };
            
#elif 0 // or

            return [delim = std::move(delim)](auto inputs)
            {
                return std::move(inputs)
              % fn::transform([delim](auto inp)
                {
                    return std::array<decltype(inp), 2>{{ std::move(inp), delim }};
                })
              % fn::concat()
              % fn::drop_last(); // drop trailing delim
            };

#else // or

            return fn::adapt([delim, flag = false](auto gen) mutable
            {
                return           !gen ? fn::end_seq() 
                     : (flag = !flag) ? gen() 
                     :                  delim;
            });
#endif
        };


        auto my_inclusive_scan = []
        {
            return fn::adapt([sum = 0](auto gen) mutable
            {
                return sum += gen();
            });
        };

        auto res = 
            fn::seq([i = 0]() mutable
            {
                return i++;
            })                  // 0,1,2,3,...

          % my_where([](int x)
            {
                return x >= 3; 
            })                  // 3,4,5,...

          % my_take_while([](int x)
            {
                return x <= 5;
            })                  // 3,4,5

          % my_intersperse(-1)  // 3,-1,4,-1,5

          % my_transform([](int x)
            {
                return x + 1;
            })                  // 4,0,5,0,6

          % my_inclusive_scan() // 4,4,9,9,15

          % fn::to_vector();

        VERIFY((res == vec_t{{4, 4, 9, 9, 15}}));
    @endcode

    A more realistic example: suppose you have a pipeline
    transforming inputs to outputs in parallel, and you want to
    compress the output, but the outputs are small and compressing
    them individually would be ineffective, and you want to 
    serialize the incoming results into a buffer of some minimum
    size, e.g. 100kb, before passing it to the compressing stage.
    @code
        auto make_result     = [](std::string s){ return s; };
        auto compress_block  = [](std::string s){ return s; };
        auto write_to_ostr   = [](std::string s){ std::cout << s; };

        namespace fn = rangeless::fn;
        using fn::operators::operator%;

        fn::seq([&]() -> std::string
        {
            std::string line;
            return std::getline(std::cin, line) ? std::move(line) 
                                                : fn::end_seq();
        })

      % fn::transform_in_parallel(make_result)

      % fn::adapt([](auto get_next)
        { 
            std::ostringstream buf{};

            while(get_next && buf.tellp() < 100000) {
                buf << get_next();
            }

            return buf.tellp() ? buf.str() : fn::end_seq();
        })

      % fn::transform_in_parallel(compress_block)

      % fn::for_each(write_to_ostr);

    @endcode
    */
    template<typename F> 
    impl::adapt<F> adapt(F fn)
    {
        return { std::move(fn) };
    }

    ///////////////////////////////////////////////////////////////////////////
    /// @brief Create a `seq` yielding results of applying the transform functions to input-elements.
    ///
    /// Returns a unary function-object, which will capture the arg by value and 
    /// return an adapted `seq<...>` which will yield results of invoking 
    /// `map_fn` on the elements (passed by value) of arg.
    /// See `fn::where` for an example.
    template<typename F> 
    impl::transform<F> transform(F map_fn)
    {
        // Rationale:
        // <br>
        // 1) A possible implementation for a container-input could assume that 
        // `transform` function is inexpensive to recompute on-demand (this is
        // this approach in `range-v3` which may call `transform` multiple times
        // per element depending on the access pattern from the downstream views).
        // 
        // 2) An implementation could require the result-type to be cacheable
        // and assume it is 'light' in terms of memory requirements and memoize the results.
        //
        // Our approach of applying `transform` lazily allows us to
        // make neither assumption and apply `transform` at most once per element and without caching. 
        // The user-code may always follow that by fn::to_vector() when necessary.

        return { std::move(map_fn) };
    }

#if 0
    // see comments around struct composed
    template<typename F, typename... Fs>
    auto transform(F fn, Fs... fns) -> impl::transform<decltype(impl::compose(std::move(fn), std::move(fns)...))>
    {
        return { impl::compose(std::move(fn), std::move(fns)...) };
    }
#endif



#if RANGELESS_FN_ENABLE_PARALLEL


    /// @brief Parallelized version of `fn::transform`
    ///
    /// Requires `#define RANGELESS_FN_ENABLE_PARALLEL 1` before `#include fn.hpp` because
    /// it brings in "heavy" STL `#include`s (`<future>` and `<thread>`).
    ///
    /// `queue_capacity` is the maximum number of simultaneosly-running `std::async`-tasks, each executing a single invocation of `map_fn`.
    /// 
    /// NB: if the execution time of `map_fn` is highly variable, having higher capacity may help, such that
    /// tasks continue to execute while we're blocked waiting on a result from a long-running task. 
    ///
    /// NB: If the tasks are too small compared to overhead of running as async-task, 
    /// it may be helpful to batch them (see `fn::in_groups_of`), have `map_fn` produce
    /// a vector of outputs from a vector of inputs, and `fn::concat` the outputs.
    /// 
    /// `map_fn` is required to be thread-safe.
    ///
    /// NB: the body of the `map_fn` would normally compute the result in-place,
    /// but it could also, for example, execute a subprocess do it, or offload it
    /// to a cloud or a compute-farm, etc.
    ///
    /// ---
    /// Q: Why do we need this? We have parallel `std::transform` and `std::transform_reduce` in c++17?
    /// 
    /// A: Parallel `std::transform` requires a multi-pass `ForwardRange`
    /// rather than `InputRange`, and `std::terminate`s if any of the tasks throws.
    /// `std::transform_reduce` requires `ForwardRange` and type-symmetric, associative, and commutative `BinaryOp`
    /// (making it next-to-useless).
    ///
    /*!
    @code
    // Example: implement parallelized gzip compressor (a-la pigz)

    #define RANGELESS_FN_ENABLE_PARALLEL 1
    #include "fn.hpp"

    #include <util/compress/stream_util.hpp>

    int main()
    {
        auto& istr = std::cin;
        auto& ostr = std::cout;

        istr.exceptions(std::ios::badbit);
        ostr.exceptions(std::ios::failbit | std::ios::badbit);

        namespace fn = rangeless::fn;
        using fn::operators::operator%;
        using bytes_t = std::string;

        fn::seq([&istr]() -> bytes_t
        {
            auto buf = bytes_t(1000000UL, '\0');
            istr.read(&buf[0], std::streamsize(buf.size()));
            buf.resize(size_t(istr.gcount()));
            return !buf.empty() ? std::move(buf) : fn::end_seq();
        })
      
      % fn::transform_in_parallel([](bytes_t buf) -> bytes_t
        {
            // compress the block.
            std::ostringstream local_ostr;
            ncbi::CCompressOStream{
                local_ostr,
                ncbi::CCompressOStream::eGZipFile } << buf;
            return local_ostr.str();

        }).queue_capacity( std::thread::hardware_concurrency() )

      % fn::for_each([&ostr](bytes_t buf)
        {
            ostr.write(buf.data(), std::streamsize(buf.size()));
        });

        return istr.eof() && !istr.bad() ? 0 : 1;
    }

    @endcode

    See an similar examples using [RaftLib](https://medium.com/cat-dev-urandom/simplifying-parallel-applications-for-c-an-example-parallel-bzip2-using-raftlib-with-performance-f69cc8f7f962)
    or [TBB](https://software.intel.com/en-us/node/506068)
    */
    template<typename F> 
    impl::par_transform<F, impl::std_async> transform_in_parallel(F map_fn)
    {
        return { impl::std_async{}, std::move(map_fn), std::thread::hardware_concurrency() };
    }

    
    /// @brief A version of `transform_in_parallel` that uses a user-provided Async (e.g. backed by a fancy work-stealing thread-pool implementation).
    ///
    /// `Async` is a unary invokable having the following signature:
    /// `template<typename NullaryInvokable> operator()(NullaryInvokable job) const -> std::future<decltype(job())>`
    ///
    /// NB: `Async` must be copy-constructible (may be passed via `std::ref` as appropriate). 
    ///
    /// A single-thread pool can be used to offload the transform-stage to a separate thread if `transform` is not parallelizeable.
    /*!
    @code
        auto res2 = 
            std::vector{{1,2,3,4,5}} // can be an InputRange

          % fn::transform_in_parallel([](auto x)
            {
                return std::to_string(x);
            },
            [&my_thread_pool](auto job) -> std::future<decltype(job())>
            {
                return my_thread_pool.enqueue(std::move(job));
            }).queue_capacity(10)

          % fn::foldl_d([](std::string out, std::string in)
            {
                return std::move(out) + "," + in;
            });
        VERIFY(res2 == ",1,2,3,4,5");
    @endcode
    */
    template<typename F, typename Async> 
    impl::par_transform<F, Async> transform_in_parallel(F map_fn, Async async)
    {
        return { std::move(async), std::move(map_fn), std::thread::hardware_concurrency() };
    }
#endif

    /// @}
    /// @defgroup folding Folds and Loops
    /// @{

    /////////////////////////////////////////////////////////////////////////////
    /*! @brief Range-based version of c++20 (copy-free) `std::accumulate`
     *
     * @see foldl_d
     * @see foldl_1
    @code
        using namespace fn::operators;
        const std::string x = vec_t{ 1, 2, 3 }
          % fn::foldl(std::string{"^"},         // init
                   [](std::string out, int in)  // fold-op
            {
                return std::move(out) + "|" + std::to_string(in);
            });

        VERIFY(x == "^|1|2|3");
    @endcode

    NB: The body of the fold-loop is `init = binary_op(std::move(init), *it);`
    If `it` is a `move`-ing iterator, like that of an `seq`, 
    you'd take `in` by value or rvalue-reference in your binary operator,
    whereas if it's a regular iterator yielding lvalue-references, you'd 
    take it by const or non-const reference.
    */
    template<typename Result, typename Op>
    impl::foldl<Result, Op> foldl(Result init, Op binary_op)
    {
        // NB: idiomatically the init value goes after the binary op,
        // but most of the time binary-op is a multiline lambda, and having
        // the init arg visually far-removed from the call-site 
        // make things less readable, i.e. foldl([](...){ many lines }, init).
        // So placing the binary-op last.
        // (Tried it both ways, and found this order preferable).

        return { std::move(init), std::move(binary_op) };
    }

    /// Fold-Left-with-Default: this version uses default-initialized value for init.
    /*!
     * @see foldl
     * @see foldl_1
    @code
        const std::string x = vec_t{ 1, 2, 3 }
        % fn::foldl_d(
            [](std::string out, int in)
            {
                return std::move(out) + "|" + std::to_string(in);
            });

        VERIFY(x == "|1|2|3");
    @endcode
    */
    template<typename Op>
    impl::foldl_d<Op> foldl_d(Op binary_op)
    {
        return { std::move(binary_op) };
    }

    /////////////////////////////////////////////////////////////////////////////
    /// Init-free version of foldl (first element is used as init); requires at least one element.
    /*!
     * @see foldl
     * @see foldl_d
     * 
    @code
        const auto min_int = 
            std::vector<std::string>{{ "11", "-333", "22" }}
        % fn::transform([](const string& s) { return std::stoi(s); })
        % fn::foldl_1([](int out, int in)   { return std::min(out, in); });

        VERIFY(min_int == -333);
    @endcode
     */
    template<typename Op>
    impl::foldl_1<Op> foldl_1(Op binary_op) 
    {
        return { std::move(binary_op) };
    }


    /// Return a `seq` yielding a view of a fixed-sized sliding window over elements.
    ///
    /// If the input is a `seq`, last `win_size` elements are internally cached in a deque.
    /*!
    @code
        const auto result = 
            make_inputs({ 1,2,3,4 })
          % fn::sliding_window(2)
          % fn::foldl(0L, [](long out, auto v)
            { 
                VERIFY(std::distance(v.begin(), v.end()) == 2);
                auto it = v.begin();
                return (out * 1000) + (*it * 10) + *std::next(it);
            });
        VERIFY(result == 12023034);
    @endcode
    */
    inline impl::sliding_window sliding_window(size_t win_size)
    {
        return { win_size };
    }

    /// Invoke fn on each element.
    ///
    /// NB: this is similar to a left-fold with binary-op `(nullptr_t, x) -> nullptr_t`
    /// where `x` is consumed by-side-effect, and the binary-op is simplified to unary `(x)->void`. 
    template<typename F>
    impl::for_each<F> for_each(F fn)
    {
        return { std::move(fn) };
    }

    /// @}
    /// @defgroup filtering Filtering
    /// @{
 
    /// @brief Yield elements until pred evaluates to false.
    template<typename P> 
    impl::take_while<P> take_while(P pred)
    {
        return { std::move(pred) };
    }

    /// @brief Yield first `n` elements.
    inline impl::take_while<impl::call_count_lt> take_first(size_t n = 1)
    {
        return {{ n, 0UL }};
    }

    /// @brief Yield last `n` elements.
    ///
    /// Buffering space requirements for `seq`: `O(n)`.
    inline impl::take_last take_last(size_t n = 1)
    {
        return { n };
    }

    /// @brief Drop elements until pred evaluates to false.
    template<typename P> 
    impl::drop_while<P> drop_while(P pred)
    {
        return { std::move(pred) };
    }

    /// @brief Drop first `n` elements.
    inline impl::drop_while<impl::call_count_lt> drop_first(size_t n = 1)
    {
        return {{ n, 0UL }};
    }

    /// @brief Drop last `n` elements.
    ///
    /// Buffering space requirements for `seq`: `O(n)`.
    inline impl::drop_last drop_last(size_t n = 1)
    {
        return { n };
    }

    ///////////////////////////////////////////////////////////////////////////
    /// @brief Filter elements.
    ///
    /// Returns a unary function-object.
    /*!
    @code
        // 1) If arg is a container passed by lvalue-reference,
        // return a copy of the container with elements 
        // satisfying the predicate:
        auto nonempty_strs = strs % fn::where([](auto&& s) { return !s.empty(); });

        // 2.1) If arg is a sequence container passed by rvalue,
        // remove the unsatisfying elements using erase-remove idiom and return the container.
        //
        // 2.2) If arg is a container having equal_range method (sets, associative containers, etc),
        // remove the unsatisfying elements using iterate-erase idiom and return the container.
        strs = std::move(strs) % fn::where([](auto&& s) { return !s.empty(); });

        // 3) If arg is a seq<G>, return adapted seq<...> that shall 
        // yield elements satisfying the predicate:
        auto res = fn::seq([i = 0]
        {
            return i < 5 ? i++ : fn::end_seq();

        }) % fn::where(     [](int x) { return x > 2; }) // 3, 4
           % fn::transform( [](int x) { return x + 1; }) // 4, 5
           % fn::foldl_d(   [](int out, int in) { return out*10 + in; });
        VERIFY(res == 45);
    @endcode
     */
    template<typename P> 
    impl::where<P> where(P pred)
    {
        return { std::move(pred) };
    }

    /// @see `where_in_sorted`
    template<typename SortedForwardRange, typename F> 
    impl::where<impl::in_sorted_by<SortedForwardRange, F> > where_in_sorted_by(const SortedForwardRange& r, F key_fn)
    {
        return { { r, false, { std::move(key_fn) } } };
    }

    ///////////////////////////////////////////////////////////////////////////
    /// @brief Intersect with a sorted range.
    ///
    /// `elems %= fn::where_in_sorted(whitelist);`
    template<typename SortedForwardRange> 
    impl::where<impl::in_sorted_by<SortedForwardRange, by::identity> > where_in_sorted(const SortedForwardRange& r)
    {
        return { { r, false, { { } } } };
    }

    /// @see `where_not_in_sorted`
    template<typename SortedForwardRange, typename F> 
    impl::where<impl::in_sorted_by<SortedForwardRange, F> > where_not_in_sorted_by(const SortedForwardRange& r, F key_fn)
    {
        return { { r, true, { std::move(key_fn) } } };
    }

    ///////////////////////////////////////////////////////////////////////////
    /// @brief Subtract a sorted range.
    ///
    /// `elems %= fn::where_not_in_sorted(blacklist);`
    template<typename SortedForwardRange> 
    impl::where<impl::in_sorted_by<SortedForwardRange, by::identity> > where_not_in_sorted(const SortedForwardRange& r)
    {
        return { { r, true, { { } } } };
    }



    ///////////////////////////////////////////////////////////////////////////
    /// @brief Filter elements to those having maximum value of fn.
    ///
    /*!
    @code
        // 1) If Arg is a container (taken by value), find the max-element
        // and return a vector of max-elements (moved-from the original container).
        strs = std::move(strs) % fn::where_max_by([](auto&& s) { return s.size(); });

        // 2) If Arg is a seq, iterates the seq and returns
        // std::vector<seq<...>::value_type> containing maximal elements.
        auto res = fn::seq([i = 0]
        {
            return i < 5 ? i++ : fn::end_seq();
        }) % fn::where_max_by([](int x) { return x % 2; });
        VERIFY((res == vec_t{{1, 3}}));
    @endcode

    Buffering space requirements for `seq`: `O(k)`, where `k` = maximum number of max-elements in a prefix of seq over all prefixes of seq.
    e.g.
    <br>`[1,0,1,1,0,1,1,9]: k = 5`
    <br>`[1,3,1,1,3,1,1,9]: k = 2`
    */
    template<typename F> 
    impl::where_max_by<F> where_max_by(F key_fn)
    {
        return { std::move(key_fn), 1 };
    }

    inline impl::where_max_by<by::identity> where_max()
    {
        return { by::identity{}, 1 };
    }

    /// Filter elements to those having maximum value of fn.
    /// @see where_max_by
    template<typename F> 
    impl::where_max_by<F> where_min_by(F key_fn)
    {
        return { std::move(key_fn), -1 };
    }

    inline impl::where_max_by<by::identity> where_min()
    {
        return { by::identity{}, -1 };
    }

    /// @code
    /// const bool any  = vals %  fn::exists_where(pred);
    /// const bool none = vals % !fn::exists_where(pred);
    /// const bool all =  vals % !fn::exists_where(not_pred);
    /// @endcode
    template<typename Pred>
    impl::exists_where<Pred> exists_where(Pred p)
    {
        return { std::move(p), true };
    }


    /// @}
    /// @defgroup grouping Grouping
    /// @{

    /// @brief Similar to `group_adjacent_by`, but presorts the elements.
    ///
    /// @see group_adjacent_by
    /*! 
    @code
        using alns_t = std::vector<CRef<CSeq_align>>;
        
        std::move(alignments)

          % fn::group_all_by([&](CConstRef<CSeq_align> aln) 
            {    
                return GetLocusId( aln->GetSeq_id(0) );
            })   

          % fn::for_each([&](alns_t alns)
            {    
                // Process alignments for a locus...
            });  

       Note: template<typename Cont> impl::group_all_by<F>::operator()(Cont inputs)
       takes Cont by-value and moves elements into the output.
    @endcode

    Buffering space requirements for `seq`: `O(N)`.
    */
    template<typename F> 
    impl::group_all_by<F> group_all_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    inline impl::group_all_by<by::identity> group_all()
    {
        return { by::identity{} };
    }

    /////////////////////////////////////////////////////////////////////////

    /// @brief Group adjacent elements.
    /// @see group_all_by
    /// @see concat
    ///
    /// If arg is a container, works similar to `group_all_by`, except 
    /// returns a `std::vector<Cont>` where each container
    /// contains adjacently-equal elements having same value of key.
    ///
    /// If arg is a `seq<...>`, composes a `seq<...>`
    /// that shall yield `vector<value_type>`s having same value of key.
    /// (NB: if the value_type is a char, the group-type is std::string).
    ///
    /// Buffering space requirements for `seq`: `O(max-groupsize)`.
    template<typename F>
    impl::group_adjacent_by<F> group_adjacent_by(F key_fn)
    {
        return { std::move(key_fn), {} };
    }

    /////////////////////////////////////////////////////////////////////////
    /// @brief Group adjacent elements.
    ///
    /// This is similar to regular `group_adjacent_by`, except the result type
    /// is a seq yielding subseqs of equivalent elements, rather than vectors.
    /// @code
    ///     fn::seq(...) 
    ///   % fn::group_adjacent_by(key_fn, fn::to_seq()) 
    ///   % fn::for_each([&](auto group_seq) // type of group_seq is a seq<...> instead of vector<...>
    ///     {
    ///         for(auto elem : group_seq) {
    ///             // ...
    ///         }
    ///     });
    /// @endcode
    ///
    /// This is useful for cases where a subseq can be arbitrarily large, and you want
    /// to process grouped elements on-the-fly without accumulating them in a vector.
    ///
    /// This comes at the cost of more constrained functionality, since all groups and
    /// all elements in each group can only be accessed once and in order.
    template<typename F>
    impl::group_adjacent_as_subseqs_by<F> group_adjacent_by(F key_fn, impl::to_seq)
    {
        return { std::move(key_fn), {} };
    }

    /////////////////////////////////////////////////////////////////////////

    inline impl::group_adjacent_by<by::identity> group_adjacent()
    {
        return { by::identity{}, {} };
    }

    inline impl::group_adjacent_as_subseqs_by<by::identity> group_adjacent(impl::to_seq)
    {
        return { by::identity{}, {} };
    }

    /////////////////////////////////////////////////////////////////////////

    /// @brief Group adjacent elements if binary predicate holds.
    template<typename BinaryPred>
    impl::group_adjacent_by<fn::by::identity, BinaryPred> group_adjacent_if(BinaryPred pred2)
    {
        return { {}, std::move(pred2) };
    }

    /// @brief Group adjacent elements if binary predicate holds.
    template<typename BinaryPred>
    impl::group_adjacent_as_subseqs_by<fn::by::identity, BinaryPred> group_adjacent_if(BinaryPred pred2, impl::to_seq)
    {
        return { {}, std::move(pred2) };
    }

    /////////////////////////////////////////////////////////////////////////


    /// @brief Group adjacent elements into chunks of specified size.
    /*!
    @code
        std::move(elems) 
      % fn::in_groups_of(5)
      % fn::for_each(auto&& group)
        {
            ASSERT(group.size() <= 5);
        });
    @endcode

    Buffering space requirements for `seq`: `O(n)`.
    */
    inline impl::group_adjacent_by<impl::chunker> in_groups_of(size_t n)
    {
        if(n < 1) {
            RANGELESS_FN_THROW("Batch size must be at least 1.");
        }
        return { impl::chunker{ n, 0 }, {} };
    }


    /// @}
    /// @defgroup ordering Ordering
    /// @{

    /// @brief Reverse the elements in the input container.
    ///
    /// Buffering space requirements for `seq`: `O(N)`.
    inline impl::reverse reverse()
    {
        return {};
    }

    /// @brief stable-sort and return the input.
    /*! 
    @code
        auto res = 
            std::list<std::string>{{ "333", "1", "22", "333", "22" }}
          % fn::to_vector()
          % fn::sort_by([](const std::string& s) 
            {
                return fn::by::decreasing(s.size());
            })
          % fn::unique_adjacent();

        VERIFY(( res == std::vector<std::string>{{ "333", "22", "1" }} ));
    @endcode

    Buffering space requirements for `seq`: `O(N)`.
    */
    template<typename F>
    impl::sort_by<F, impl::stable_sort_tag> sort_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    /// @brief `sort_by with key_fn = by::identity`
    inline impl::sort_by<by::identity, impl::stable_sort_tag> sort()
    {
        return { by::identity{} };
    }


    template<typename F>
    impl::sort_by<F, impl::unstable_sort_tag> unstable_sort_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    inline impl::sort_by<by::identity, impl::unstable_sort_tag> unstable_sort()
    {
        return { by::identity{} };
    }


    /// @brief Unstable lazy sort.
    ///
    /// Initially move all inputs into a `std::vector` and heapify in `O(n)`,
    /// and then lazily yield elements with `pop_heap` in `O(log(n))`. This is more efficient
    /// if the downstream stage is expected to consume a small fraction of
    /// sorted inputs.
    ///
    /// Buffering space requirements for `seq`: `O(N)`.
    template<typename F>
    impl::lazy_sort_by<F> lazy_sort_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    /// @brief `lazy_sort_by with key_fn = by::identity`
    inline impl::lazy_sort_by<by::identity> lazy_sort()
    {
        return { by::identity{} };
    }

    /// @brief Return top-n elements, sorted by key_fn.
    ///
    /// This is conceptually similar to `fn::sort_by(key_fn) % fn::take_last(n)`,
    /// but is more efficient: the implementation mantains a priority-queue of
    /// top-n max-elements encountered so-far.
    ///
    /// Buffering space requirements: `O(n)`; time complexity: `O(N*log(n))`
    template<typename F>
    impl::take_top_n_by<F> take_top_n_by(size_t n, F key_fn)
    {
        return { std::move(key_fn), n };
    }

    /// @brief Return top-n elements, sorted by identity.
    inline impl::take_top_n_by<by::identity> take_top_n(size_t n)
    {
        return { by::identity{}, n };
    }



    /// @}
    /// @defgroup uniq Uniquefying
    /// @{

    /// @brief Keep first element from every adjacently-equal run of elements.
    /// 
    /// If arg is a container, apply erase-unique idiom and return
    /// the container.
    ///
    /// If arg is a `seq<In>`, compose `seq<Out>` that
    /// will yield first element from every adjacently-equal run of elements.
    template<typename F>
    impl::unique_adjacent_by<F> unique_adjacent_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    inline impl::unique_adjacent_by<by::identity> unique_adjacent()
    {
        return { by::identity{} };
    }

    /// @brief Uniquefy elements globally, as-if `unique_adjacent_by` pre-sorted by same key.
    ///
    /// If arg is a container, move contents to vector, and then sort-by and unique-adjacent-by.
    ///
    /// If arg is a `seq<In>`, compose `seq<Out>` that will
    /// keep values of key_fn in a set, and skip already-seen elements. 
    ///
    /// NB: the lifetime of value returned by key_fn must be independent of arg.
    template<typename F>
    impl::unique_all_by<F> unique_all_by(F key_fn)
    {
        return { std::move(key_fn) };
    }

    inline impl::unique_all_by<by::identity> unique_all()
    {
        return { by::identity{} };
    }    
    

    /// @}
    /// @defgroup concat Combining
    /// @{

    /// @brief Flatten the result of `group_all_by` or `group_adjacent_by`. 
    ///
    /// Given a `ContainerA<ContainerB<...>>`, move all elements and return `ContainerB<...>`.
    ///
    /// Given a seq-of-iterables `seq<In>`, compose `seq<Out>` that will lazily yield elements 
    /// of each iterable in seq.
    inline impl::concat concat()
    {
        return {};
    }

    /// @brief Yield elements of next after elements of arg.
    /*!
    @code
        auto res = vec_t{{1,2}}
                 % fn::append( vec_t{{3}} )
                 % fn::to_vector();

        VERIFY((res == vec_t{{1,2,3}}));
    @endcode
    */
    template<typename Iterable>
    impl::append<Iterable> append(Iterable next)
    {
        return { std::move(next) };
    }

    /// @brief Yield pairs of elements, (up to end of the shorter range)
    /*!
    @code
        auto res = vec_t{{1,2}}
                 % fn::zip_with( vec_t{{3,4,5}}, [](int x, int y)
                   {
                        return x*10 + y;
                   })
                 % fn::to_vector();

        VERIFY((res == vec_t{{13, 24}}));
    @endcode
    */
    template<typename Iterable, typename BinaryFn>
    impl::zip_with<Iterable, BinaryFn> zip_with(Iterable second, BinaryFn fn)
    {
        // We could have provided zip2 instead, returning a pair of elements,
        // but more often than not that pair will need to be transformed,
        // so instead we provide generalized zip_with that also takes the
        // binary transform-function, obviating the need in separate transform-stage.
        // If necessary, the transform-function can simply yield std::make_pair.

        return { std::move(second), std::move(fn) };
    }

    /// @brief Yield invocations of `fn` over pairs of adjacent inputs.
    template<typename BinaryFn>
    impl::zip_adjacent<BinaryFn> zip_adjacent(BinaryFn fn)
    {
        return { std::move(fn) };
    }

    // @brief Outer zip_with.
    template<typename Iterable, typename BinaryFn>
    impl::cartesian_product_with<Iterable, BinaryFn> cartesian_product_with(Iterable second, BinaryFn fn)
    {
        return { std::move(second), std::move(fn) };
    }

    /// @}
    /// @defgroup el_access Element Access
    /// @{

    /////////////////////////////////////////////////////////////////////////////
    /*! @brief Access unique element matching the predicate.
     *
     * Throw unless found exactly one element matching the predicate.
     * Returns const or non-const reference depending on the constness of the container.
     * @see set_unique
    @code
        const CConstRef<CUser_object>& model_evidence_uo = 
            fn::get_unique(seq_feat.GetExts(), [](CConstRef<CUser_object> uo)
            {
                return uo->GetType().GetStr() == "ModelEvidence";
            });
    @endcode
    */
    template<typename Container, typename P>
    auto get_unique(Container& container, P&& pred) -> decltype(*container.begin())
    {
        auto best_it = container.end();
        size_t n = 0;

        for(auto it   = container.begin(), 
                 it_end = container.end();

                 it != it_end; ++it)
        {
            if(pred(*it)) {
                best_it = it;
                ++n;
            }
        }

        if(n != 1) {
            RANGELESS_FN_THROW("Expected unique element satisfying search criteria, found " 
                          + std::to_string(n) + ".");
        }

        return *best_it;
    }

    /////////////////////////////////////////////////////////////////////////////
    /// @brief Similar to get_unique, but end-insert an element if missing.
    ///
    /*!
    @code
        CRef<CSeqdesc>& gb_desc =
            fn::set_unique(seq_entry.SetDescr().Set(), [](CConstRef<CSeqdesc> d)
            {    
                return d->IsGenbank();
            },   
            [] // add if missing (must satisfy pred)
            {    
                auto g = Ref(new CSeqdesc);
                g->SetGenbank();
                return g;
            });  
    @endcode
    */
    template<typename Container, typename P, typename Construct>
    auto set_unique(Container& container, P&& pred, Construct&& con) -> decltype(*container.begin())
    {
        auto best_it = container.end();
        size_t n = 0;

        for(auto it   = container.begin(), 
                 it_end = container.end();

                 it != it_end; ++it)
        {
            if(pred(*it)) {
                best_it = it;
                ++n;
            }
        }

        if(n == 0) {
            best_it = container.insert(container.end(), con());
            ++n;
            if(!pred(*best_it)) {
                RANGELESS_FN_THROW("New element does not satisfy predicate.");
            }
        }

        if(n != 1) {
            RANGELESS_FN_THROW("Expected unique element satisfying search criteria, found " 
                          + std::to_string(n) + ".");
        }

        return *best_it;
    }


    /////////////////////////////////////////////////////////////////////////////

    /// e.g. `const CConstRef<CSeq_align> aln = first_or_default( get_alns_annot(...)->Get() );`
    template<typename Container>
    auto first_or_default(const Container& c) -> typename Container::value_type
    {
        return c.begin() == c.end() ? typename Container::value_type{} : *c.begin();
    }

    template<typename Container, typename Pred>
    auto first_or_default(const Container& c, Pred&& pred) -> typename Container::value_type
    {
        auto it = std::find_if(c.begin(), c.end(), std::forward<Pred>(pred));
        return it == c.end() ? typename Container::value_type{} : *it;
    }

    template<typename Container>
    auto last_or_default(const Container& c) -> typename Container::value_type
    {
        return c.rbegin() == c.rend() ? typename Container::value_type{} : *c.rbegin();
    }

    template<typename Container, typename Pred>
    auto last_or_default(const Container& c, Pred&& pred) -> typename Container::value_type
    {
        auto it = std::find_if(c.rbegin(), c.rend(), std::forward<Pred>(pred));
        return it == c.rend() ? typename Container::value_type{} : *it;
    }

    /// @}
    /// @defgroup other Util
    /// @{

    /// @brief Memoizing wrapper for non-recursive non-mutable unary lambdas (not synchronized).
    /*!
    @code
        size_t exec_count = 0;
        auto fn = make_memoized([&](int x){ exec_count++; return x*2; });
        
        VERIFY(fn(1) == 2);
        VERIFY(fn(2) == 4);
        VERIFY(fn(1) == 2);
        VERIFY(exec_count == 2);
    @endcode
    */
    template<typename F>
    impl::memoizer<F> make_memoized(F fn)
    {
        return { std::move(fn), {} };
    }

    /////////////////////////////////////////////////////////////////////////
    /// @brief Basic scope guard - execute some code in guard`s destructor.
    /*!
    @code
        {{
            // ...

            auto on_exit = fn::make_scope_guard([&]
            {
                std::cerr << "Executing clean-up or roll-back...\n";
            });

            on_exit.dismiss(); // Cancel if no longer applicable.
        }}
    @endcode
    */
    template<typename F>
    impl::scope_guard<F> make_scope_guard(F fn)
    {
        return { std::move(fn), false };
    }

    /// @}


namespace operators
{
    // Why operator % ?
    // Because many other libraries overload >> or | for various purposes,
    // so in case of compilation errors involving those 
    // you get an honorable mention of every possible overload in TU.
    //
    // Also, in range-v3 it is used for composition of views
    // rather than a function application, so we want to avoid possible confusion.

    /// @brief `return std::forward<F>(fn)(std::forward<Arg>(arg))`
    ///
    /// This is similar to Haskell's operator `(&) :: a -> (a -> b) -> b |infix 1|` or F#'s operator `|>`
    
#if 0 && (defined(__GNUC__) || defined(__clang__))
    __attribute__ ((warn_unused_result))
    // A user may forget to iterate over the sequence, e.g.
    //         std::move(inputs) % fn::transform(...); // creates and immediately destroys a lazy-seq.
    // whereas the user code probably intended:
    //         std::move(inputs) % fn::transform(...) % fn::for_each(...);
    // 
    // To deal with this we could make operator% nodiscard / warn_unused_result,
    // but disabling for now because I can't figure out how to suppress the 
    // warning from the final % for_each(...).
#endif 
    template<typename Arg, typename F>
    auto operator % (Arg&& arg, F&& fn) -> decltype( std::forward<F>(fn)(std::forward<Arg>(arg)) )
    {
        // NB: forwarding fn too because it may have rvalue-specific overloads
        return std::forward<F>(fn)(std::forward<Arg>(arg));
    }

    // Note: in operator %= and <<= below we're not returning the original reference
    // because the operators are right-to-left associative, so it makes no sense to do it.

    /// @brief `arg = fn::to(Arg{})(std::forward<F>(fn)(std::move(arg)));`
    ///
    /// `strs %= fn::where([](const std::string& s){ return !s.empty(); }); // drop empty strings`
    template<typename Arg, typename F>
    auto operator %= (Arg& arg, F&& fn) -> decltype(void(std::forward<F>(fn)(std::move(arg))))
    {
        // NB: forwarding fn too because it may have rvalue-specific overloads.
        // Note: result-type may be not the same as arg, so we 
        // have to move the elements rather than simply assign.
        arg = fn::to(Arg{})(std::forward<F>(fn)(std::move(arg)));
    }

    /// @brief End-insert elements of cont2 into cont1 by-move.
    /*!
    @code
        out <<= GetItems();

        // equivalent of:
        {{
            auto items = GetItems();
            for(auto&& item : items) {
                out.insert(out.end(), std::move(item));
            }
        }}
    @endcode
    */
    template<class Container1, class Container2>
    auto operator<<=(Container1& cont1, 
                    Container2&& cont2) -> decltype(void(cont1.insert(cont1.end(), std::move(*cont2.begin()))))
    {
        static_assert(std::is_rvalue_reference<Container2&&>::value, "");
        cont1.insert(cont1.end(),
                     std::make_move_iterator(cont2.begin()),
                     std::make_move_iterator(cont2.end()));
    }

    // Could also take Container2 by value, but taking by rvalue-reference instead
    // will preserve the internal storage of the container (instead of it being 
    // freed here when going out of scope if we took it by value). In some cases
    // this may be desirable. So instead having separate overloads for 
    // Container2&& and const Container2& that will copy elements.

    template<class Container1, class Container2>
    auto operator<<=(Container1& cont1, 
               const Container2& cont2) -> decltype(void(cont1.insert(cont1.end(), *cont2.begin())))
    {
        cont1.insert(cont1.end(),
                     cont2.begin(),
                     cont2.end());
    }

    // I would have expected that seq-based input would work with the above overload as well,
    // but that is not so - after wrapping into std::make_move_iterator we're getting 
    // compilation errors about missing operators +,+=,-,-= for the iterator.
    // Hence the special overload for seq (it already yields rvalue-references)
    template<class Container1, class Gen>
    void operator<<=(Container1& cont1, impl::seq<Gen> seq)
    {
        cont1.insert(cont1.end(), seq.begin(), seq.end());
    }

    template<class Container>
    void operator<<=(Container& cont, typename Container::value_type el)
    {
         cont.insert(cont.end(), std::move(el));
    }

} // namespace operators

} // namespace fn

} // namespace rangeless
        


#if RANGELESS_FN_ENABLE_RUN_TESTS
#include <list>
#include <string>
#include <tuple>
#include <set>
#include <array>
#include <iostream>
#include <sstream>
#include <cctype>

#ifndef VERIFY
#define VERIFY(expr) if(!(expr)) RANGELESS_FN_THROW("Assertion failed: ( "#expr" ).");
#endif

namespace rangeless
{
namespace fn
{
namespace impl
{

// move-only non-default-constructible type wrapping an int.
// Will use it as our value_type in tests to verify that 
// we're supporting this.
struct X
{
    int value;

    X(int i) : value{ i }
    {}

#if defined(__GNUC__) && (__GNUC___ >= 5)
    X() = delete;

    // We want to test that everything compiles even if the
    // type is not default-constructibse, however, with GCC 4.9.3
    // that results in
    // /opt/ncbi/gcc/4.9.3/include/c++/4.9.3/bits/stl_algobase.h:573:4: error: static assertion failed: type is not assignable
    // when calling std::erase or std::rotate on a vector<X>.
    //
    // So making leaving X default-constructible.
#endif
               X(X&&) = default;
    X& operator=(X&&) = default;

               X(const X&) = delete;
    X& operator=(const X&) = delete;

    operator int&()
    {
        return value;
    }

    operator const int& () const
    {
        return value;
    }
};
using Xs = std::vector<X>;

// This is templated to unary-callable, because
// we want to reuse the same battery of tests when
// the input is a container and when the input-type
// is a lazy seq.
template<typename UnaryCallable>
auto make_tests(UnaryCallable make_inputs) -> std::map<std::string, std::function<void()>>
{
    std::map<std::string, std::function<void()>> tests{};
    using fn::operators::operator%;

    //make_inputs({1, 2, 3}); // returns Xs or seq yielding X's

    auto fold = fn::foldl_d([](int64_t out, int64_t in)
    {
        return out*10 + in;
    });


#if 0
    tests["single test of interest"] = [&]
    {

    };
#else 


    /////////////////////////////////////////////////////////////////////////

    tests["Test for NB[3]"] = [&]
    {
        auto res = make_inputs({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20})
        % fn::transform([](X x) { return std::to_string(x); })
        % fn::foldl_d([](std::string out, std::string in)
        {
            return std::move(out) + "," + in;
        });
        VERIFY(res == ",1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20");
    };

    tests["foldl"] = [&]
    {
        // ensure compilation with move-only inputs
        auto res = make_inputs({1,2,3})
      % fn::foldl(X{42}, [](X out, int in)
        {
            return X(out*10+in);
        });
        VERIFY(res == 42123);
    };

    tests["transform_in_parallel"] = [&]
    {
        auto res = make_inputs({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20})
        % fn::transform_in_parallel([](X x) { return x; }).queue_capacity(10)
        % fn::foldl_d([](std::string out, X in)
        {
            return std::move(out) + "," + std::to_string(in);
        });
        VERIFY(res == ",1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20");


#if __cplusplus >= 201402L
        auto res2 = make_inputs({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20})
        % fn::transform_in_parallel([](X x) { return std::to_string(x); },
                                    [](auto job)
                                    {
                                        return std::async(std::launch::async, std::move(job));
                                    }).queue_capacity(10)
        % fn::foldl_d([](std::string out, std::string in)
        {
            return std::move(out) + "," + in;
        });
        VERIFY(res2 == ",1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20");
#endif
    };



    /////////////////////////////////////////////////////////////////////////

    tests["sliding_window"] = [&]
    {
        //using view_t = fn::view<typename vec_t::iterator>;
        // TODO: does not compile if using auto instead of view_t

#if __cplusplus >= 201402L
    // using auto-parameter for `v` in lambda, can be
    // either container's iterator, or deque<...>::iterator 
    // depending on inputs type

        const auto result = 
            make_inputs({ 1,2,3,4 })
          % fn::sliding_window(2)
          % fn::foldl(0L, [](long out, auto v)
            { 
                VERIFY(std::distance(v.begin(), v.end()) == 2);
                auto it = v.begin();
                return (out * 1000) + (*it * 10) + *std::next(it);
            });
        VERIFY(result == 12023034);
#endif
    };

    /////////////////////////////////////////////////////////////////////////

    tests["where"] = [&]
    {
        auto ret = make_inputs({1,2,3}) 
            % fn::where([](int x) { return x != 2; }) 
            % fold;
        VERIFY(ret == 13);

        //123 % fn::where([](int x) { return x == 2; }); 
    };


    tests["where_in_sorted"] = [&]
    {
        auto ret = make_inputs({1,2,3,4})
           % fn::where_in_sorted(std::vector<int>{{ 1, 3}})
           % fold;
        VERIFY(ret == 13);
    };

    tests["where_not_in_sorted"] = [&]
    {
        auto ret = make_inputs({1,2,3,4})
           % fn::where_not_in_sorted(std::vector<int>{{1, 3}})
           % fold;
        VERIFY(ret == 24);
    };

    tests["where_max_by"] = [&]
    {
        auto ret = make_inputs({1,3,1,3}) 
            % fn::where_max_by(fn::by::identity{})
            % fold;
        VERIFY(ret == 33);
    };

    tests["where_min_by"] = [&]
    {
        // NB: also testing compilation where key_fn returns a tuple
        // containing references (see NB[2])
        auto ret = make_inputs({5,3,5,3}) 
            % fn::where_min_by([](const int& x) { return std::tie(x); })
            % fold;
        VERIFY(ret == 33);
    };

    tests["take_while"] = [&]
    {
        auto ret = make_inputs({3,4,1,2}) 
            % fn::take_while([](int x) { return x > 1; })
            % fold;
        VERIFY(ret == 34);
    };

    tests["drop_while"] = [&]
    {
        auto ret = make_inputs({3,4,1,2}) 
            % fn::drop_while([](int x) { return x > 1; })
            % fold;
        VERIFY(ret == 12);
    };


    tests["take_last"] = [&]
    {
        auto ret = 
              make_inputs({1,2,3}) 
            % fn::take_last(2)
            % fold;
        VERIFY(ret == 23);

        ret = make_inputs({1,2,3}) 
            % fn::take_last(3)
            % fold;
        VERIFY(ret == 123);

        ret = make_inputs({1,2,3}) 
            % fn::take_last(4)
            % fold;
        VERIFY(ret == 123);

        const auto inputs = std::vector<int>{{1,2,3}};
        ret = inputs % fn::take_last(2) % fold;
        VERIFY(ret == 23);
    };

    tests["drop_last"] = [&]
    {
        auto ret = 
              make_inputs({1,2,3}) 
            % fn::drop_last(2)
            % fold;

        VERIFY(ret == 1);

        ret = make_inputs({1,2,3}) 
            % fn::drop_last(3)
            % fold;

        VERIFY(ret == 0);

        ret = make_inputs({1,2,3}) 
            % fn::drop_last(4)
            % fold;

        VERIFY(ret == 0);

        const auto inputs = std::vector<int>{{1,2,3}};
        ret = inputs % fn::drop_last(2) % fold;
        VERIFY(ret == 1);
    };

    tests["take_first"] = [&]
    {
        auto ret = 
              make_inputs({1,2,3}) 
            % fn::take_first(2)
            % fold;
        VERIFY(ret == 12);
    };

    tests["drop_first"] = [&]
    {
        auto ret = 
              make_inputs({1,2,3}) 
            % fn::drop_first(2)
            % fold;
        VERIFY(ret == 3);
    };


    /////////////////////////////////////////////////////////////////////////

    tests["append"] = [&]
    {
        auto res = make_inputs({1,2,3})
                 % fn::append( make_inputs({4,5}) )
                 % fold;

        VERIFY(res == 12345);
    };

    tests["zip_with"] = [&]
    {
        auto res = make_inputs({1,2})
                 % fn::zip_with( make_inputs({3,4,5}), [](int x, int y)
                   {
                        return std::array<int, 2>{{x, y}};
                   })
                 % fn::concat()
                 % fold;

        VERIFY(res == 1324);
    };


    /////////////////////////////////////////////////////////////////////////

    tests["enumerate"] = [&]
    {
        auto res = make_inputs({4,5,6})
        % fn::transform(fn::get::enumerated{})
        % fn::transform([](std::pair<size_t, int> p)
        {
            return std::array<int, 2>{{p.second, int(p.first)}};
        })
        % fn::concat()
        % fold;

        VERIFY(res == 405162);
    };

    /////////////////////////////////////////////////////////////////////////
    
    tests["values"] = [&]
    {
        auto inps = make_inputs({1,2,3});

        auto m = std::map<int, decltype(inps) >{};
        m.emplace(1, std::move(inps));
        m.emplace(2, make_inputs({4,5,6}));

        auto res = std::move(m) 
                 % fn::transform(fn::get::second{})
                 % fn::concat() 
                 % fold;

        VERIFY(res == 123456);
    };

    /////////////////////////////////////////////////////////////////////////

    tests["group_adjacent"] = [&]
    {
        auto res = make_inputs({1,2,2,3,3,3,2,2,1})
        % fn::group_adjacent()
        % fn::transform([](Xs v)
        {
            return int(v.size());
        })
        % fold;
        VERIFY(res == 12321);
    };

    tests["group_adjacent_if"] = [&]
    {
        auto res = make_inputs({1,2,2,4,4,4,2,2,1})
        % fn::group_adjacent_if([](const int& a, const int& b)
        {
            return abs(a - b) < 2;
        })
        % fn::transform([](Xs v)
        {
            return int(v.size());
        })
        % fold;
        VERIFY(res == 333);
    };

    tests["group_all"] = [&]
    {
        auto res = make_inputs({1,2,2,3,3,3,2,2,1})
        % fn::group_all()
        % fn::transform([](Xs v)
        {
            return int(v.size());
        })
        % fold;
        VERIFY(res == 243);
    };

    tests["in_groups_of"] = [&]
    {
        auto res = make_inputs({1,2,3,4,5})
        % fn::in_groups_of(2)
        % fn::transform([](Xs v)
        {
            return v.size();
        })
        % fold;
        VERIFY(res == 221);
    };

    tests["concat"] = [&]
    {
        auto res = make_inputs({1,2,2,3,3,3,2,2,1})
        % fn::group_all()
        % fn::concat()
        % fold;
        VERIFY(res == 112222333);
    };

    tests["unique_adjacent"] = [&]
    {
        auto res = make_inputs({1,2,2,3,3,3,2,2,1})
        % fn::unique_adjacent()
        % fold;
        VERIFY(res == 12321);
    };

    tests["unique_all"] = [&]
    {
        // NB: key-function requires copy-constructible in this use-case.
        // see unique_all_by::gen::operator()()
        auto res = make_inputs({1,2,2,3,3,3,2,2,1})
#if 0
        % fn::unique_all()
        // Normally this would work, but we've made our type X not default-constructible,
        // so when using identity key-fn, the type X returned by it cannot be 
        // used as key-type to store seen elements. Instead, will use modified key-fn below.
#elif 1
        % fn::unique_all_by([](const X& x) -> const int& { return x; })
#elif 0
        % fn::unique_all_by([](const X& x) { const int& r = x; return std::tie(r); })
        // This is not expected to work because returned reference-wrapper is
        // not default-constructible.
#endif
        % fold;
        VERIFY(res == 123);
    };

    tests["sort"] = [&]
    {
        auto res = make_inputs({3,2,4,1,5})
        % sort()
        % reverse()
        % fold;
        VERIFY(res == 54321);

        res = make_inputs({3,2,4,1,5})
        % unstable_sort()
        % fold;
        VERIFY(res == 12345);
    };

    tests["lazy_sort"] = [&]
    {
        auto res = make_inputs({3,2,4,1,5})
        % lazy_sort()
        % reverse()
        % fold;
        VERIFY(res == 54321);
    };

    tests["top_n"] = [&]
    {
        auto res = make_inputs({3,2,4,1,5,0})
        % take_top_n(3)
        % fold;
        VERIFY(res == 345);
    };


#if __cplusplus >= 201402L
    tests["group_adjacent_as_subseqs"] = [&]
    {
        int res = 0;
        int group_num = 0;

        make_inputs({1,2,2,3,3,3,4,5})
      % fn::group_adjacent(fn::to_seq())
      % fn::for_each([&](auto subseq)
        {
            //subseq.size();  // sanity-check: this should not compile, because type is seq<...>

            group_num++;
            if(group_num == 4) {
                return; // testing skipping a group without accessing it
            }

            size_t i = 0;
            for(const auto x : subseq) {

                // testing that things work as expected
                // for partially-accessed groups leaving
                // some elements unaccessed. Here we'll skip
                // the last 3 in 333 subgroup.
                if(i++ >= 2) {
                    break;
                }

                res = res*10 + x;
            }
            res = res*10;
        });

        VERIFY(res == 1022033050);
    };
#endif


    tests["key_fn returning a reference_wrapper"] = [&]
    {
        // testing compilation. see lt
        auto get_ref = [](const X& x) { return std::cref(x); };

        make_inputs({1,2,3}) %       fn::group_all_by(get_ref);
        make_inputs({1,2,3}) %  fn::group_adjacent_by(get_ref);
        make_inputs({1,2,3}) %            fn::sort_by(get_ref);
        make_inputs({1,2,3}) % fn::unique_adjacent_by(get_ref);
    //  make_inputs({1,2,3}) %      fn::unique_all_by(get_ref);
    //  This is static-asserted against for seq because the implementation
    //  requires default-constructible key-type (see discussion in unique_all_by::gen)
    };

    tests["to"] = [&]
    {
        auto res = make_inputs({2,3,1,2}) % fn::to(std::set<int>{}) % fold;
        VERIFY(res == 123);
    };

    /////////////////////////////////////////////////////////////////////////
#endif

    return tests;
}


/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
static void run_tests()
{
    using fn::operators::operator%;

#if 0 // single test

    std::map<std::string, std::function<void()>> tests1{}, tests2{}, test_other{};
    test_other["single"] = [&]
    {
        auto res = Xs{}
        % fn::to_seq()
        % fn::group_all()
        % fn::transform([](Xs v)
        {
            return (int)v.size();
        });
    };

#else // all tests

    std::map<std::string, std::function<void()>> 
        test_cont{}, test_seq{}, test_view{}, test_other{};

    // make battery of tests where input is a container (Xs)
    test_cont = make_tests([](std::initializer_list<int> xs)
    {
        Xs ret;
        for(auto x : xs) {
            ret.push_back(X(x));
        }
        return ret;
    });

    // make battery of tests where input is an input-range
    test_seq = make_tests([](std::initializer_list<int> xs)
    {
        Xs ret;
        for(auto x : xs) {
            ret.push_back(X(x));
        }
        return fn::to_seq()(std::move(ret));
    });

    // make battery of tests where input is a view
    test_view = make_tests([](std::initializer_list<int> xs)
    {
        // will be returning views; will keep the underlying data
        // in a static vector.

        static std::vector<Xs> vecs{};
        vecs.push_back(Xs{});
        auto& ret = vecs.back();

        for(auto x : xs) {
            ret.push_back(X(x));
        }
        return fn::from(ret);
    });


    using vec_t = std::vector<int>;

    /////////////////////////////////////////////////////////////////////////


    /////////////////////////////////////////////////////////////////////////

    test_other["any_seq_t"] = [&]
    {
        fn::any_seq_t<int> myseq = 
            fn::seq([i = 0]() mutable
            {
                return i < 10 ? i++ : fn::end_seq();
            })
          % fn::where([](int x)
            {
                return x % 2 == 0;
            });

        int res = 0;
        for(const auto& x : myseq) {
            res = res*10 + x;
        }
       
        VERIFY(res == 2468);
    };

    test_other["for_each"] = [&]
    {
        // NB: making sure that for_each compiles with mutable lambdas
        int res = 0;
        vec_t{{1,2,3}} % fn::for_each([&res](int& x) mutable
        {
            res = res*10 + x;
        });
        VERIFY(res == 123);
    };

    test_other["zip_adjacent"] = [&]
    {
        auto res =
            vec_t{{1,2,3,4}}
          % fn::zip_adjacent([](int& x1, int& x2) // making sure non-const-reference binds
            {
                return x1*10 + x2;
            })
          % fn::to_vector();

        VERIFY(( res == std::vector<int>{{12,23,34}} ));
    };

    test_other["fold"] = [&]
    {
        const std::string x = 
        vec_t{{1,2,3}} % fn::foldl(std::string{"^"},
                                [](std::string s, int x_)
        {
            return std::move(s) + "|" + std::to_string(x_);
        }) ;
        VERIFY(x == "^|1|2|3");
    };

    test_other["foldl_d"] = [&]
    {
        const std::string x =
        vec_t{{1,2,3}} % fn::foldl_d( [](std::string s, int x_)
        {
            return std::move(s) + "|" + std::to_string(x_);
        });

        VERIFY(x == "|1|2|3");
    };

    test_other["foldl_1"] = [&]
    {
        const auto min_int = 
            std::vector<std::string>{{ "11", "-333", "22" }}
        % fn::transform([](const std::string& s) { return std::stoi(s); })
        % fn::foldl_1([](int out, int in) { return std::min(out, in); });

        VERIFY(min_int == -333);
    };


    /////////////////////////////////////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////////
    test_other["fn::where with associative containers"] = [&]
    {
        // verify what filtering works for containers where
        // std::remove_if is not supported (e.g. assotiative)
        
        auto ints2 = std::set<int>{{111, 333}};

        using fn::operators::operator%=;

        ints2 %= fn::where([](int x) { return x > 222; });
        VERIFY(ints2.size() == 1 && *ints2.begin() == 333);

        using map_t = std::map<int, int>;
        auto m = map_t{{ {1,111}, {3,333} }};

        m %= fn::where([](const map_t::value_type& x) { return x.second > 222; });
        VERIFY(m.size() == 1 && m.at(3) == 333);

        //*m.begin() = *m.begin();
    };

    test_other["fn::where with mutable lambdas"] = [&]
    {
         // must work with mutable lambda
         int i = 0;
         auto ints2 = vec_t{{1,2,3}} % fn::where([i](int) mutable { return i++ >= 1; });
         VERIFY(( ints2 == vec_t{{2, 3}} ));
    };

    test_other["fn::where with const_reference inputs"] = [&]
    {
        // check const-reference overload with non-sequence containers
        const auto ints1 = std::set<int>{1,2,3};
        auto ints2 = ints1 % fn::where([](int x_) { return x_ > 1; });
        VERIFY(( ints2.size() == 2 ));
    };

    test_other["fn::where with non-const reference inputs"] = [&]
    {
        // check const-reference overload with non-sequence containers
        auto ints1 = std::set<int>{1,2,3};
        auto ints2 = ints1 % fn::where([](int x_) { return x_ > 1; });

        VERIFY(( ints1.size() == 3 ));
        VERIFY(( ints2.size() == 2 ));
    };


    test_other["refs"] = [&]
    {
        auto inps = vec_t({1,2,3});

        auto res = fn::refs(inps)
           % fn::foldl_d([](int64_t out, int& in)
             {
                ++in;
                // NB: taking in by non-const reference to verify
                // that it binds to non-const reference-wrapper
                return out*10 + in;
             });
        VERIFY(res == 234);

        // check with const inputs
        const auto& const_inps = inps;
        res = fn::refs(const_inps)
           % fn::foldl_d([](int64_t out, const int& in)
             {
                return out*10 + in;
             });
        VERIFY(res == 234);

    };


    /////////////////////////////////////////////////////////////////////////


    test_other["memoized"] = [&]
    {
        size_t exec_count = 0;
        auto strs = std::vector<std::string>{{ "333", "4444", "22", "1" }};

        using fn::operators::operator%=;
        strs %= fn::sort_by(fn::make_memoized([&](const std::string& s)
        {
            exec_count++; 
            return s.size();
        }));

        VERIFY((strs == std::vector<std::string>{{ "1", "22", "333", "4444" }}));
        VERIFY(exec_count == 4);
    };

    test_other["scope_guard"] = [&]
    {
        int i = 0;
        {{
            auto on_exit_add1 = fn::make_scope_guard([&]
            {
                i += 1;
            });

            auto on_exit_add10 = fn::make_scope_guard([&]
            {
                i += 10;
            });

            on_exit_add1.dismiss();
        }}
        VERIFY(i == 10);
    };

    test_other["operator<<=, operator%="] = [&]
    {
        using fn::operators::operator<<=;
        using fn::operators::operator%=;

        auto cont = vec_t{{1,2}};
        cont <<= std::set<int>{{3,4}}; // excerising rvalue-based overload

        const auto const_set = std::set<int>{{5,6}};
        cont <<= const_set; // excercising conts-reference overload.

        int i = 0;
        cont <<= fn::seq([&i]{ return i++ < 1 ? 7 : fn::end_seq(); }); // should also work for seq rhs

        cont <<= 9;

        cont %= fn::where([](int x) { return x % 2 != 0; });

        VERIFY((cont == vec_t{{1,3,5,7,9}}));
    };

#if 0
    test_other["generate and output_iterator"] = []
    {
        int i = 0;
        auto r = fn::seq([&i]
        {
            return i < 5 ? i++ : fn::end_seq();
        });

        int sum = 0;
        std::copy(r.begin(), r.end(), fn::make_output_iterator([&](int x)
        {
            sum = sum * 10 + x;
        }));

        VERIFY(sum == 1234);
    };
#endif

    test_other["get_unique/set_unique"] = [&]
    {
        vec_t ints{{1,2,3}};

        auto& x1 = get_unique(ints, [](int x_){ return x_ == 2; });
        VERIFY(x1 == 2);

        // verify that works with const
        const auto& const_ints = ints;
        const auto& x2 = get_unique(const_ints, [](int x_){ return x_ == 2; });
        VERIFY(&x1 == &x2);

        auto ints2 = ints;
        auto& y = set_unique(ints2, [](int x_) { return x_ == 42; },
                                    []{ return 42; });

        VERIFY( ints2.size() == 4 
            &&  ints2.back() == 42 
            && &ints2.back() == &y);
    };

    test_other["group_adjacent_by vector-storage-recycling"] = []
    {
        // Check that vec_t& passed by reference
        // is reused on subsequent iterations instead 
        // of reallocated de-novo.
        std::set<int*> ptrs;

        auto result = 
            vec_t{2,4,1,3,5,6,7,9}
          % fn::to_seq()
          % fn::group_adjacent_by([](int x) { return x % 2; })
          % fn::foldl_d([&](int64_t out, vec_t&& in)
            {
                in.reserve(64);
                ptrs.insert(in.data());
                return out*10 + in.size();
            });

        VERIFY((result == 2312));
        VERIFY((ptrs.size() <= 2));
    };

    test_other["decreasing"] = [&]
    {
        // sort by longest-first, then lexicographically

        const auto inp      = std::vector<std::string>{ "2", "333", "1", "222", "3" };
        const auto expected = std::vector<std::string>{ "222", "333", "1", "2", "3" };

        auto ret1 = inp % fn::sort_by([](const std::string& s)
        {
            return std::make_tuple(fn::by::decreasing(s.size()), std::ref(s));
        });
        VERIFY(ret1 == expected);

        auto ret2 = inp % fn::sort_by([](const std::string& s)
        {
            return std::make_tuple(s.size(), fn::by::decreasing(std::ref(s)));
        }) % fn::reverse() % fn::to_vector();

        VERIFY(ret2 == expected);

        auto ret3 = inp % fn::sort_by(fn::by::flipped([](const std::string& s)
        {
            return std::make_tuple(s.size(), fn::by::decreasing(std::ref(s)));
        }));

        VERIFY(ret3 == expected);

        auto ret4 = inp;
        std::sort(ret4.begin(), ret4.end(), fn::by::make_comp([](const std::string& s)
        {
            return std::make_tuple(fn::by::decreasing(s.size()), std::ref(s));
        }));

        VERIFY(ret4 == expected);
    };

    test_other["by"] = [&]
    {
        // make sure first{}, second{}, and dereferenced{} return arg as reference (or reference-wrapper)
        // rather than by value.

        std::vector<std::pair<std::unique_ptr<int>, int>> v{};
        
        fn::sort_by(fn::by::first{})(std::move(v));
        fn::sort_by(fn::by::second{})(std::move(v));

        const auto xy = std::make_pair(10, 20);
        const auto& x = fn::by::first{}(xy);
        VERIFY(&x == &xy.first);

        std::vector<std::unique_ptr<int> > v2{};
        fn::sort_by(fn::by::dereferenced{})(std::move(v2));
    };

    test_other["exists_where"] = [&]
    { 
         VERIFY(  ( vec_t{{ 1, 2, 3}} %  fn::exists_where([](int x){ return x == 2; }) ));
         VERIFY( !( vec_t{{ 1, 2, 3}} %  fn::exists_where([](int x){ return x == 5; }) ));
         VERIFY(  ( vec_t{{ 1, 2, 3}} % !fn::exists_where([](int x){ return x == 5; }) ));
    };


#if __cplusplus >= 201402L
    test_other["compose"] = []
    {
        auto my_transform = [](auto fn)
        {
            return fn::adapt([fn = std::move(fn)](auto gen)
            {
                return fn(gen());
            });
        };

        auto my_where = [](auto pred)
        {
            return fn::adapt([pred = std::move(pred)](auto gen)
            {
                auto x = gen();
                while(!pred(x)) {
                    x = gen();
                }
                return x;
            });
        };

        auto my_take_while = [](auto pred)
        {
            return fn::adapt([pred = std::move(pred)](auto gen)
            {
                auto x = gen();
                return pred(x) ? std::move(x) : fn::end_seq();
            });
        };

        auto my_intersperse = [](auto delim)
        {
#if 1
            return [delim = std::move(delim)](auto inputs)
            {
                return fn::seq([  delim, 
                                 inputs = std::move(inputs), 
                                     it = inputs.end(), 
                                started = false, 
                                   flag = false]() mutable
                {
                    if(!started) {
                        started = true;
                        it = inputs.begin();
                    }
                    return it == inputs.end() ? fn::end_seq()
                         :     (flag = !flag) ? std::move(*it++)
                         :                      delim;
                });
            };
            
#elif 0 // or

            return [delim = std::move(delim)](auto inputs)
            {
                return std::move(inputs)
              % fn::transform([delim](auto inp)
                {
                    return std::array<decltype(inp), 2>{{ delim, std::move(inp) }};
                })
              % fn::concat()
              % fn::drop_first(); // drop leading delim
            };

#else // or

            return fn::adapt([delim, flag = false](auto gen) mutable
            {
                return           !gen ? fn::end_seq() 
                     : (flag = !flag) ? gen() 
                     :                  delim;
            });
#endif
        };

        auto my_inclusive_scan = []
        {
            return fn::adapt([sum = 0](auto gen) mutable
            {
                return sum += gen();
            });
        };

        auto res = 
            fn::seq([i = 0]() mutable
            {
                return i++;
            })                  // 0,1,2,3,...

          % my_where([](int x)
            {
                return x >= 3; 
            })                  // 3,4,5,...

          % my_take_while([](int x)
            {
                return x <= 5;
            })                  // 3,4,5

          % my_intersperse(-1)  // 3,-1,4,-1,5

          % my_transform([](int x)
            {
                return x + 1;
            })                  // 4,0,5,0,6

          % my_inclusive_scan() // 4,4,9,9,15

          % fn::to_vector();

        VERIFY((res == vec_t{{4, 4, 9, 9, 15}}));
    };
#endif // tests for compose


    test_other["cartesian_product_with"] = [&]
    {
        auto res = 
            vec_t{{1,2}}
          % fn::cartesian_product_with( vec_t{{3,4,5}}, [](int x, int y)
            {
                return x*10+y;
            })
          % fn::foldl_d([](int64_t out, int64_t in)
            {
                return out*1000+in;
            });

         VERIFY(res == 13014015023024025);
    };

    test_other["guard_against_multiple_iterations_of_input_range"] = [&]
    {
        auto gen_ints = [](int i, int j)
        {
            return fn::seq([i, j]() mutable
            {
                return i < j ? i++ : fn::end_seq();
            });
        };

        auto xs =
            gen_ints(0, 6) 
          % fn::transform([&](int x) { return gen_ints(x, x+2); })
          % fn::concat()
          % fn::where([](int x) { return x % 2 == 0; });

        // 011223344556
        int64_t res = 0;
        bool threw = true;
        try {
            // xs is a seq, so trying to iterate it 
            // a second time shall throw.
            for(size_t i = 0; i < 2; i++) {
                for(auto x : xs) {
                    res = res*10 + x;
                }
            }
            threw = false;
        } catch(...) {
            threw = true;
        }
        VERIFY(threw);
        VERIFY(res == 22446);
    };


    test_other["most 5 top frequent words"] = [&]
    {
        // TODO : for now just testing compilation

        std::istringstream istr{};

        auto my_isalnum = [](const int ch)
        {
            return std::isalnum(ch) || ch == '_';
        };

        using counts_t = std::map<std::string, size_t>;

        fn::from(
            std::istreambuf_iterator<char>(istr.rdbuf()),
            std::istreambuf_iterator<char>{})

          % fn::transform([](const char c) // tolower
            {
                return ('A' <= c && c <= 'Z') ? char(c - ('Z' - 'z')) : c;
            })

          % fn::group_adjacent_by(my_isalnum)

#if 0
            // build word->count map
          % fn::foldl_d([&](counts_t out, const std::string& in)
            {
                if(my_isalnum(in.front())) {
                    ++out[ in ];
                }
                return std::move(out);
            })
#else
          % fn::where([&](const std::string& s)
            {
                return my_isalnum(s.front());
            })
          % fn::counts() // map:word->count
#endif

          % fn::group_all_by([](const counts_t::value_type kv)
            {
                return kv.first.size(); // by word-size
            })

          % fn::transform(
                  fn::take_top_n_by(5UL, fn::by::second{}))

          % fn::concat()

          % fn::for_each([](const counts_t::value_type& kv)
            {
                std::cerr << kv.first << "\t" << kv.second << "\n";
            })
          ;
    };

    test_other["placement-new without std::launder"] = [&]
    {
        struct S
        {
            const int n;
            const int& r;
        };
        const int x1 = 1;
        const int x2 = 2;

        impl::maybe<S> m{};

        m.reset(S{42, x1});
        VERIFY((*m).n == 42);
        VERIFY((*m).r == 1);

        m.reset(S{420, x2});
        VERIFY((*m).n == 420);
        VERIFY((*m).r == 2);
    };

    test_other["counts"] = [&]
    {
        auto res = 
            vec_t{{ 1, 1,2, 1,2,3 }} 
          % fn::counts()
          % fn::transform([](const auto& kv)
            {
                return kv.first * 10 + int(kv.second);
            })
          % fn::to_vector();
        
        VERIFY((res == vec_t{{13, 22, 31}}));
    };


#endif // single-test vs all

    /////////////////////////////////////////////////////////////////////////
    size_t num_failed = 0;
    size_t num_ok = 0;
    for(auto&& tests : { test_cont, test_seq, test_view, test_other })
        for(const auto& kv : tests)
    {
        try{
            kv.second();
            num_ok++;
        } catch(const std::exception& e) {
            num_failed++;
            std::cerr << "Failed test '" << kv.first << "' :" << e.what() << "\n";
        }
    }
    if(num_failed == 0) {
        std::cerr << "Ran " << num_ok << " tests - OK\n";
    } else {
        throw std::runtime_error(std::to_string(num_failed) + " tests failed.");
    }
}

} // namespace impl
} // namespace fn
} // namespacce rangeless


#endif //RANGELESS_FN_ENABLE_RUN_TESTS

#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

#endif // #ifndef RANGELESS_FN_HPP_
