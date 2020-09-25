# rangeless::fn
## `range`-free LINQ-like library of higher-order functions for manipulation of containers and lazy input-sequences.


### [Documentation](https://ast-al.github.io/rangeless/docs/html/namespacerangeless_1_1fn.html)


### What it's for
- Reduce the amount of mutable state.
- Flatten control-flow.
- Lessen the need to deal with iterators directly.
- Make the code more expressive and composeable.

This library is intended for moderate to advanced-level c++ programmers that like the idea of c++ `ranges`, but can't or choose not to use them for various reasons (e.g. high complexity, compilation overhead, debug-build performance, size of the library, etc).

Motivations:
- https://www.fluentcpp.com/2019/09/13/the-surprising-limitations-of-c-ranges-beyond-trivial-use-cases/
- https://brevzin.github.io/c++/2020/07/06/split-view/
- http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2011r1.html
- https://aras-p.info/blog/2018/12/28/Modern-C-Lamentations/


### Features
- Portable c++11. (examples are c++14)
- Single-header.
- Minimal standard library dependencies.
- No inheritance, polymorphism, type-erasures, ADL, advanced metaprogramming, enable_ifs, concepts, preprocessor magic, arcane programming techniques (for some definition of arcane), or compiler-specific workarounds.
- Low `#include` and compile-time overhead.
- Enables trivial parallelization (see [`fn::to_async and fn::transform_in_parallel`](https://ast-al.github.io/rangeless/docs/html/group__parallel.html)).
- Allows for trivial extension of functionality (see [`fn::adapt`](https://ast-al.github.io/rangeless/docs/html/group__transform.html)).


### Simple examples
```cpp
namespace fn = rangeless::fn;

// A common complaint among c++ programmers is the verbosity of lambdas, 
// which are used a lot with this library. The macro can to alleviate it somewhat. 
// I use it here just for demonstration and am personally ambivalent about it. 
// It is not defined in the library.
#define L(expr) ([&](const auto& _ ){ return expr; })

struct employee_t
{
            int id;
    std::string last_name;
    std::string first_name;
            int years_onboard;

    bool operator<(const employee_t& other) const
    {
        return id < other.id;
    }
};


auto employees = std::vector<employee_t>{/*...*/};

employees = fn::where L( _.last_name != "Doe" )(                   std::move(employees) );
employees = fn::take_top_n_by(10, L( _.years_onboard ))(           std::move(employees) );
employees = fn::sort_by L( std::tie( _.last_name, _.first_name) )( std::move(employees) );

// or, as single nested function call:

employees = fn::sort_by L( std::tie( _.last_name, _.first_name))(
                fn::take_top_n_by(10, L( _.years_onboard ))(
                    fn::where L( _.last_name != "Doe" )(
                        std::move(employees) )));
```

How does this work? E.g. `fn::sort_by(projection_fn)` is a higher-order function that returns a unary function that takes inputs by value (normally passed as rvalue), sorts them by the user-provided projection, and returns them by value.

`operator %`, invoked as `arg % unary_function`, is syntax-sugar, similar to F#'s operator `|>`, that enables structuring your code in top-down manner, consistent with the direction of the data-flow, similar to UNIX pipes. It is implemented as:
```cpp
    template<typename Arg, typename F>
    auto operator % (Arg&& arg, F&& fn) -> decltype( std::forward<F>(fn)( std::forward<Arg>(arg)) )
    {
        return std::forward<F>(fn)( std::forward<Arg>(arg));
    }
```

The original example can then be written as:
```cpp

using fn::operators::operator%;
using fn::operators::operator%=;  // arg %= fn; // arg = fn( std::move(arg));

employees %= fn::where L( _.last_name != "Doe" );
employees %= fn::take_top_n_by(10, L( _.years_onboard ));
employees %= fn::sort_by L( std::tie( _.last_name, _.first_name) );

// or:

employees = std::move(employees)
          % fn::where L( _.last_name != "Doe" )
          % fn::take_top_n_by(10, L( _.years_onboard ))
          % fn::sort_by L( std::tie( _.last_name, _.first_name) );
```


### Example: Top-5 most frequent words chosen among the words of the same length.
```cpp

    auto my_isalnum = [](const int ch)
    {    
        return std::isalnum(ch) || ch == '_'; 
    };   

    using counts_t = std::map<std::string, size_t>;

    fn::from(
        std::istreambuf_iterator<char>(istr.rdbuf()),
        std::istreambuf_iterator<char>{})
        
      % fn::transform L( ('A' <= _ && _ <= 'Z') ? char(_ - ('Z' - 'z')) : _ ) // to-lower
      % fn::group_adjacent_by(my_isalnum)             // returns sequence-of-std::string
      % fn::where L( my_isalnum( _.front()))          // discard strings with punctuation
      % fn::counts()                                  // returns map<string,size_t> of word->count
      % fn::group_all_by L( _.first.size())           // returns [[(word, count)]], each subvector containing words of same length
      % fn::transform(                                // transform each sub-vector...
            fn::take_top_n_by(5UL, fn::by::second{})) // by filtering it taking top-5 by count.
      % fn::concat()                                  // undo group_all_by (flatten)
      % fn::for_each( [](const counts_t::value_type& kv)
        {    
            std::cout << kv.first << "\t" << kv.second << "\n";
        })   
      ; 

    // compilation time:
    // >>time g++ -I ../include/ -std=c++14 -o test.o -c test.cpp
    // real   0m1.176s
    // user   0m1.051s
    // sys    0m0.097s
```

#### More examples
- [A rudimentary lazy TSV parser](https://godbolt.org/z/f6eptu).
- [calendar.cpp](test/calendar.cpp) vs. [Haskell](https://github.com/BartoszMilewski/Calendar/blob/master/Main.hs) vs. [range-v3 implementation](https://github.com/ericniebler/range-v3/blob/master/example/calendar.cpp).
- [aln_filter.cpp](test/aln_filter.cpp) for more advanced examples of use.

### Description  

Unlike `range-v3`, this library is centered around value-semantics rather than reference-semantics. This library does not know or deal with the multitude of range-related concepts; rather, it deals with data transformations via higher-order functions. It differentiates between two types of inputs: a `Container` and a lazy `seq<NullaryInvokable>` satisfying single-pass forward-only `InputRange` semantics (also known as a data-stream). Most of the function-objects in this library have two overloads of `operator()` respectively. Rather than composing views over ranges as with `range-v3`, `operator()`s take inputs by value, operate on it eagerly or compose a lazy `seq`, as appropriate (following the Principle of Least Astonishment), and return the result by value (with move-semantics) to the next stage.

E.g.
- `fn::where`
  - given a container, passed by rvalue, returns the same container filtered to elements satisfying the predicate.
  - given a container, passed by lvalue-reference, returns a copy of the container with elements satisfying the predicate.
  - given a `seq`, passed by value, composes and returns a `seq` that will skip the elements not satisfying the predicate.
- `fn::sort`
  - given a container, passed by value, returns the sorted container.
  - given a `seq`, passed by value, moves elements into a `std::vector`, and delegates to the above.
- `fn::transform`
  - given a `seq`, passed by value, returns a `seq` wrapping a composition of the transform-function over the underlying `NullaryInvokable`.
  - given a container, passed by value, wraps it as `seq` and delegates to the above.


Some functions in this library internally buffer elements, as appropriate, with single-pass streaming inputs, whereas `range-v3`, on the other hand, imposes multipass ForwardRange or stronger requirement on the inputs in situations that would otherwise require buffering. This makes this library conceptually more similar to UNIX pipelines with eager `sort` and lazy `sed`, than to c++ ranges.

| Operations | Buffering behavior | Laziness |
| ---------- | ------------------ | -------- |
| `fn::group_adjacent_by`, `fn::in_groups_of` | buffer elements of the incoming group | lazy |
| `fn::unique_all_by` | buffer unique keys of elements seen so far | lazy |
| `fn::drop_last`, `fn::sliding_window` | buffer a queue of last `n` elements | lazy |
| `fn::transform_in_parallel` | buffer a queue of `n` executing async-tasks | lazy |
| `fn::group_all_by`, `fn::sort_by`, `fn::lazy_sort_by`, `fn::reverse`, `fn::to_vector` | buffer all elements | eager |
| `fn::take_last` | buffer a queue of last `n` elements | eager |
| `fn::where_max_by`, `fn::where_min_by` | buffer maximal/minimal elements as seen so-far | eager |
| `fn::take_top_n_by` | buffer top `n` elements as seen so-far | eager |


### Signaling `end-of-sequence` from a generator-function

More often than not a generator-function that yields a sequence of values will not be an infinite Fibonacci sequence, but rather some bounded sequence of objects, either from a file, a socket, a database query, etc, so we need to be able to signal end-of-sequence. One way to do it is to yield elements wrapped in `std::unique_ptr` or `std::optional`:
```cpp
  fn::seq([]() -> std::unique_ptr<...> { ... })
% fn::take_while([](const auto& x) { return bool(x); })
% fn::transform(fn::get::dereferenced{})
% ...
```
If your value-type has an "empty" state interpretable as end-of-inputs, you can use the value-type directly without wrapping.

If you don't care about incurring an exception-handling overhead once per whole seq, there's a simpler way of doing it: just return `fn::end_seq()` from the generator function (e.g. see my_intersperse example). This throws end-of-sequence exception that is caught under the hood (python-style). If you are in `-fno-exceptions` land, then this method is not for you.


### Summary of different ways of passing inputs

```cpp
      fn::seq([]{ ... }) % ... // as input-range from a nullary invokable

          std::move(vec) % ... // pass a container by-move
                    vec  % ... // pass by-copy

           fn::from(vec) % ... // as move-view yielding elements by-move (std::move will make copies iff vec is const)
          fn::cfrom(vec) % ... // as above, except always take as const-reference / yield by copy
           fn::refs(vec) % ... // as seq taking vec by reference and yielding reference-wrappers

fn::from(it_beg, it_end) % ... // as a move-view into range (std::move will make copies iff const_iterator)
  fn::from(beg_end_pair) % ... // as above, as std::pair of iterators
```
Note: `fn::from` can also be used to adapt an lvalue-reference to an `Iterable` that implements
`begin()` and `end()` as free-functions rather than methods.


### Primer on using projections
Groping/sorting/uniqing/where_max_by/etc. functions take a projection function rather than a binary comparator as in `std::` algorithms.
```cpp

    // Sort by employee_t::operator<.
    employees %= fn::sort(); // same as fn::sort_by( fn::by::identity{} );

    // Sort by a projection involving multiple fields (first by last_name, then by first_name):
    employees %= fn::sort_by L( std::make_pair( _.last_name, _.first_name ));

    // The above may be inefficient (makes copies); prefer returning as tuple of references:
    employees %= fn::sort_by L( std::tie( _.last_name, _.first_name ));

    // If need to create a mixed tuple capturing lvalues as references and rvalues as values:
    employees %= fn::sort_by L( std::make_tuple( _.last_name.size(), std::ref( _.last_name ), std::ref( _.first_name )));

    // Equivalently, using capture_as_tuple that captures lvalues as references and rvalues as values:
#define L_TUPLE(...) ([&](const auto& _ ){ return fn::capture_as_tuple(__VA_ARGS__); })
    employees %= fn::sort_by L_TUPLE( _.last_name.size(), _.last_name, _.first_name );

    // fn::by::decreasing() and fn::by::decreasing_ref() can wrap individual values or references,
    // The wrapper captures the value or reference and exposes inverted operator<.
    // E.g. to sort by (last_name's length, last_name descending, first_name):
    employees %= fn::sort_by L_TUPLE( _.last_name.size(), fn::by::decreasing_ref( _.last_name ), _.first_name ));

    // fn::by::decreasing() can also wrap the entire projection-function:
    employees %= fn::sort_by( fn::by::decreasing L_TUPLE( _.last_name.size(), _.last_name, _.first_name ));

    // If the projection function is expensive, and you want to invoke it once per element:
    auto expensive_key_fn = [](const employee_t& e) { return ... };

    employees = std::move(employees)
              % fn::transform([&](employee_t e)
                {
                    auto key = expensive_key_fn(e);
                    return std::make_pair( std::move(e), std::move(key) );
                })
              % fn::sort_by( fn::by::second{})   // or L( std::ref( _.second ))
              % fn::transform( fn::get::first{}) // or L( std::move( _.first ))
              % fn::to_vector();

    // Alternatively, expensive_key_fn can be wrapped with a unary-function-memoizer
    // (results are internally cached in std::map and subsequent lookups are log-time).
    employees %= fn::sort_by( fn::make_memoized( expensive_key_fn ));

    // fn::make_comp() takes a projection and creates a binary Comparator object
    // that can be passed to algorithms that require one.
    gfx::timsort( employees.begin(), employees.end(),
                  fn::by::make_comp L( std::tie( _.last_name, _.first_name )));
```

### `#include` and compilation-time overhead

Despite packing a lot of functionality, `#include <fn.hpp>` adds only a tiny sliver (~0.03s) of compilation-time overhead in addition to the few common standard library include-s that it relies upon:

```cpp
// tmp.cpp

#if defined(STD_INCLUDES)
#    include <stdexcept>
#    include <algorithm>
#    include <functional>
#    include <vector>
#    include <map>
#    include <deque>
#    include <string>
#    include <cassert>
#elif defined(INCLUDE_FN)
#    include "fn.hpp"
#elif defined(INCLUDE_RANGE_ALL)
#    include <range/v3/all.hpp>
#endif

int main()
{
    return 0;
}
```

```sh
# just std-includes used by fn.hpp
>>time for i in {1..10}; do g++ -std=c++14 -DSTD_INCLUDES=1 -o tmp.o -c tmp.cpp; done
real    0m3.682s
user    0m3.106s
sys     0m0.499s

# fn.hpp
>>time for i in {1..10}; do g++ -std=c++14 -DINCLUDE_FN=1 -o tmp.o -c tmp.cpp; done
real    0m3.887s
user    0m3.268s
sys     0m0.546s

# range/v3/all.hpp, for comparison
>>time for i in {1..10}; do g++ -std=c++14 -DINCLUDE_RANGE_ALL=1 -I. -o tmp.o -c tmp.cpp; done
real    0m22.687s
user    0m20.412s
sys     0m2.043s
```

There are not many compiler-torturing metaprogramming techniques used by the library, so the template-instantiation overhead is reasonable as well (see the Top-5 most frequent word example; leaving it as an excercise to the reader to compare with raw-STL-based implementation).


### Discussion

Many programmers after getting introduced to toy examples get an impression that
the intended usage is "to express the intent" or "better syntax" or to "separate the concerns", etc.  
Others look at the toy examples and point out that they could be straightforwardly written
as normal imperative code, and I tend to agree with them:
There's no real reason to write code like:
```cpp
    std::move(xs)
  % fn::where(     [](const auto& x) { return x % 2 == 0; })
  % fn::transform( [](const auto& x) { return x * x; }
  % fn::for_each(  [](const auto& x) { std::cout << x << "\n"; })
```

, if it can be written simply as
```cpp
for(const auto& x : xs)
    if(x % 2 == 0)
        std::cerr << x*x << "\n";
```
The "functional-style" equivalent just incurs additional compile-time, debugging-layers, and possibly run-time overhead.

There are some scenarios where functional-style is useful:

#### Const-correctness and reduction of mutable state.
When you declare a non-const variable in your code (or worse, when you have to deal with an API that forces you to do that),
you introduce another "moving part" in your program. Having more things const makes your code more robust and easier to reason about.

With this library you can express complex data transformations as a single expression, assigning the result to a const variable
(unless you intend to `std::move()` it, of course).

```
const auto result = std::move(inputs) % stage1 % stage2 % ... ;
```

Another code-pattern for this is immediately-executed-lambda.
```cpp
const X x = [&]
{
    X x{};

    // build X...
    return x;
}();
```

#### Reduced boilerplate.
E.g. compare
```cpp
employees %= fn::where L( _.last_name != "Doe" );
```
vs. idiomatic c++ way:
```cpp
employees.erase(
    std::remove_if( employees.begin(), 
                    employees.end(), 
                    [](const employee_t& e)
                    {
                        return e.last_name == "Doe"; 
                    }), 
    employees.end());
```

#### Transformations over infinite (arbitrarily large) streams (`InputRange`s)

The most useful use-case is the scenarios for writing a functional pipeline
over an infinite stream that you want to manipulate lazily, like a UNIX pipeline, e.g. the above-mentioned [aln_filter.cpp](test/aln_filter.cpp).

#### Implement embarassingly-parallel problems trivially.

Replacing `fn::transform` with `fn::transform_in_parallel` where appropriate may be all it takes to parallelize your code.

### Downsides and Caveats.
Compilation errors related to templates are completely gnarly.
For this reason the library has many `static_asserts` to help you figure things out. If you encounter a compilation error that could benefit from adding a `static_assert`, please open an issue.


Sometimes it may be difficult to reason about the complexity space and time requirements of some operations. There are two ways to approach this: 1) Peek into documentation where I discuss space and time big-O for cases that are not obvious (e.g. how `lazy_sort_by` differs from regular `sort_by`, or how `unique_all_by` operates in single-pass for `seq`s). 2) Feel free to peek under the hood of the library. Most of the code is intermediate-level c++ that should be within the ability to grasp by someone familiar with STL and `<algorithm>`.


There's a possibility that a user may instantiate a `seq` and then forget to actually iterate over it, e.g.
```cpp
    std::move(inputs) % fn::transform(...); // creates and immediately destroys a lazy-seq.

    // whereas the user code probably intended:
    std::move(inputs) % fn::transform(...) % fn::for_each(...);
```

### Minimum supported compilers: MSVC-19.15, GCC-4.9.3, clang-3.7, ICC-18

### References:
- [Haskell Data.List](https://hackage.haskell.org/package/base-4.12.0.0/docs/Data-List.html)
- [Scala LazyList](https://www.scala-lang.org/api/2.13.x/scala/collection/immutable/LazyList.html)
- [Elixir Stream](https://hexdocs.pm/elixir/Stream.html)
- [Elm List](https://package.elm-lang.org/packages/elm/core/latest/List)
- [O'Caml List](https://caml.inria.fr/pub/docs/manual-ocaml/libref/List.html)
- [F# Collections.Seq](https://msdn.microsoft.com/en-us/visualfsharpdocs/conceptual/collections.seq-module-%5bfsharp%5d)
- [D std.range](https://dlang.org/phobos/std_range.html)
- [Rust Iterator](https://doc.rust-lang.org/std/iter/trait.Iterator.html)

c++ -specific (in no particular order):
- [c++20 std::ranges](https://en.cppreference.com/w/cpp/experimental/ranges)
- [tcbrindle/NanoRange](https://github.com/tcbrindle/NanoRange)
- [ericniebler/range-v3](https://github.com/ericniebler/range-v3)
- [Dobiasd/FunctionalPlus](https://github.com/Dobiasd/FunctionalPlus)
- [jscheiny/Streams](https://github.com/jscheiny/Streams)
- [A Lazy Stream Implementation in C++11](https://www.codeproject.com/Articles/512935/A-Lazy-Stream-Implementation-in-Cplusplus)
- [ryanhaining/CPPItertools](https://github.com/ryanhaining/cppitertools)
- [soheilhy/fn](https://github.com/soheilhy/fn)
- [LoopPerfect/conduit](https://github.com/LoopPerfect/conduit)
- [k06a/boolinq](https://github.com/k06a/boolinq)
- [pfultz2/linq](https://github.com/pfultz2/linq)
- [boost.range](https://www.boost.org/doc/libs/1_67_0/libs/range/doc/html/index.html)
- [cpplinq](https://archive.codeplex.com/?p=cpplinq)
- [simonask/rx-ranges](https://github.com/simonask/rx-ranges)
- [ReactiveX/RxCpp](https://github.com/ReactiveX/RxCpp)
- [arximboldi/zug](https://github.com/arximboldi/zug)
- [MarcDirven/cpp-lazy](https://github.com/MarcDirven/cpp-lazy)
- [qnope/Little-Type-Library](https://github.com/qnope/Little-Type-Library)

Recommended blogs:
- [Eric Niebler](https://ericniebler.com)
- [fluent c++](https://www.fluentcpp.com)
- [Andrzej's C++ blog](https://akrzemi1.wordpress.com)
- [foonathan::blog()](https://foonathan.net)
- [quuxplusone](https://quuxplusone.github.io/blog)
- [Bartosz Milewski](https://bartoszmilewski.com)
