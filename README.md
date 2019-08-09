# rangeless::fn
## `range`-free LINQ-like library of higher-order functions for manipulation of containers and lazy input-sequences.

- Reduce the amount of mutable state.
- Flatten control-flow.
- Lessen the need to deal with iterators directly.
- Make the code more expressive and composeable.

This library is intended for moderate to advanced-level c++ programmers that like the idea of c++ `ranges`, but can't use them for various reasons (high complexity, compilation overhead, debug-build performance, size of the library, etc).


### Simple examples
```cpp
namespace fn = rangeless::fn;
using fn::operators::operator%;   // arg % f % g % h; // h(g(f(std::forward<Arg>(arg))));
using fn::operators::operator%=;  // arg %= fn;       // arg = fn(std::move(arg));

employees %= fn::sort_by([](const auto& e){ return std::tie(e.last_name, e.first_name); });
             // this returns a unary function xs->xs that sorts by the given predicate.

employees %= fn::where([](const auto& e){ return e.last_name != "Doe"; });
             // say Good Riddance to the erase-remove idiom and iterate-erase loops.

employees %= fn::take_top_n_by(10, [](const auto& e){ return e.years_onboard; });

// or 

employees = std::move(employees)
          % fn::sort_by(           [](const auto& e){ return std::tie(e.last_name, e.first_name); })
          % fn::where(             [](const auto& e){ return e.last_name != "Doe"; })
          % fn::take_top_n_by(10,  [](const auto& e){ return e.years_onboard; });
```

```cpp
    // 
    // Top-5 most frequent words among the words of the same length.
    //
    auto my_isalnum = [](const int ch)
    {
        return std::isalnum(ch) || ch == '_';
    };

    fn::move_from(
        std::istreambuf_iterator<char>(std::cin.rdbuf()),
        std::istreambuf_iterator<char>{})

      % fn::transform([](const char ch) { return std::tolower(uint8_t(ch)); })

      % fn::group_adjacent_by(my_isalnum)

        // build word->count map
      % fn::foldl_d([&](std::map<std::string, size_t> out, const auto& w)
        {
            if(my_isalnum(w.front())) {
                ++out[ std::string(w.begin(), w.end()) ];
            }
            return std::move(out);
        })

      % fn::group_all_by([](const auto& kv) { return kv.first.size(); })

      % fn::transform(fn::take_top_n_by(5UL, fn::by::second{}))

      % fn::concat()

      % fn::for_each([](const auto& kv)
        {
            std::cerr << kv.first << "\t" << kv.second << "\n";
        })
      ;

    // compilation time:
    // >>time g++ -I ../include/ -std=c++14 -o test.o -c test.cpp
    // real	0m1.176s
    // user	0m1.051s
    // sys	0m0.097s
```

See [calendar.cpp](test/calendar.cpp) vs. [Haskell](https://github.com/BartoszMilewski/Calendar/blob/master/Main.hs) vs. [range-v3 implementation](https://github.com/ericniebler/range-v3/blob/master/example/calendar.cpp).

See [aln_filter.cpp](test/aln_filter.cpp) for more advanced examples of use.

See [full documentation](https://ast-al.github.io/rangeless/html/namespacerangeless_1_1fn.html).
  
### Features
- Portable c++11. (examples are c++14)
- Single-header.
- Minimal standard library dependencies.
- No inheritance, polymorphism, type-erasures, ADL, advanced metaprogramming, enable_ifs, concepts, preprocessor magic, arcane programming techniques (for some definition of arcane), or compiler-specific workarounds.
- Low `#include` and compile-time overhead.
- Enables trivial parallelization (see [`fn::transform_in_parallel`](https://ast-al.github.io/rangeless/html/group__transform.html)).
- Allows for trivial extension of functionality (see [`fn::adapt`](https://ast-al.github.io/rangeless/html/group__transform.html)).

This is not a range library, like `range-v3`, as it is centered around value-semantics rather than reference-semantics. This library does not know or deal with the multitude of range concepts; rather, it deals with data transformations via higher-order functions. It differentiates between two types of inputs: a `Container` and a lazy `seq<NullaryInvokable>` satisfying single-pass forward-only `InputRange` semantics (also known as a data-stream). Most of the function-objects in this library have two overloads of `operator()` respectively. Rather than composing views over ranges as with `range-v3`, `operator()`s take inputs by value, operate on it eagerly or compose a lazy `seq`, as appropriate (following the Principle of least astonishment), and return the result by value (with move-semantics) to the next stage.

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


Some functions in this library internally buffer elements, as appropriate, with single-pass streaming inputs, whereas `range-v3`, on the other hand, imposes multipass ForwardRange or stronger requirement on the inputs in situations that would otherwise require buffering. This makes this library conceptually more similar to UNIX pipelines than to c++ ranges.

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

### Discussion
There seems to be a lot of misunderstanding about the intended use-cases for this style of coding.

Many programmers after getting introduced to toy examples get an impression that
the intended usage is "to express the intent" or "better syntax" or to "separate the concerns", etc.  
Others look at the toy examples and point out that they could be straightforwardly written
as normal imperative code, and I tend to agree with them:
Never write a code like:
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
If you are doing "functional style" for no good reason, you are doing it wrong - there's no value-added
and you are paiyng compile-time, debugging-layers, and possibly run-time overhead.

There are some scenarios where functional-style is useful:

#### Const-correctness.
By this I mean: when an lvalue is in the process of being built, it should be considered write only;
when it is is being used, it should be read-only (declared const and enforced by the compiler, unless
the intent is to `std::move` it).
e.g. a contrived example
```cpp
map<std::string, size_t> word_counts{};

// modify the map of word-counts...
// ...
// ... many of code-lines later, that may or may not have modified word_counts...
// ...
// access word_counts

```
If `word_counts` could be declared const, you could ignore all the code
that may-or-may-not modify word_counts between declaration and usage, e.g.:
```cpp
const auto word_counts = words % fn::fold_d([](map<std::string, size_t> m, const auto& word)
{
    ++m[word];
    return std::move(m);
};
```

Another code-pattern for this is immediately-executed-lambda.
```cpp
const X x = [&]
{
    X x{}; // this shadows the outside-x above, and it's GOOD, 
           // as we don't want to accidently access it here.

    // build X...
    return x;
}();
```

#### Reduction of mutable state.
Non-composeable API force the programmer to declare mutable state that really should be temporary.
e.g.
```cpp
auto values = get_values(); // has to be non-const, because will sort
std::sort(values.begin(), values.end());
```
whereas a composeable API does not necessitate a mutation of an lvalue:
```cpp
const auto sorted_values = get_values() % fn::sort();
```

#### Erase-remove idiom.
If you feel that the idiomatic way of filtering a container is an abomination that should have never seen the light of day, then you'll find this library useful for that alone.

#### Transformations over infinite (arbitrarily large) streams (`InputRange`s)

The most useful use-case is the scenarios for writing a function or an expression
where the input is an infinite stream that you want to manipulate lazily, like a UNIX pipeline, e.g. the above-mentioned [aln_filter.cpp](test/aln_filter.cpp).

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
- [boost.range](https://www.boost.org/doc/libs/1_67_0/libs/range/doc/html/index.html)
- [cpplinq](https://archive.codeplex.com/?p=cpplinq)


Recommended blogs:
- [Eric Niebler](https://ericniebler.com)
- [fluent c++](https://www.fluentcpp.com)
- [Andrzej's C++ blog](https://akrzemi1.wordpress.com)
- [foonathan::blog()](https://foonathan.net)
- [quuxplusone](https://quuxplusone.github.io/blog)
- [Bartosz Milewski](https://bartoszmilewski.com)
