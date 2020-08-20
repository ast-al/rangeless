#define RANGELESS_FN_ENABLE_PARALLEL 1
#include <fn.hpp>
#include <iostream>

// A real-world -inspired example showcasing same of the fn:: functionality:
//
// Problem Statement:
//
// Given a stream of mrna-to-chromosome alignments (aln_t), sorted by gene-id, 
// filter it as follows, lazily, i.e. fetching from the stream incrementally as-necessary:
//
// Per gene_id:
//      1) Drop alignments where exists an alignment with seq-id 
//         having same mrna-accession and higher mrna-version,
//         e.g. drop ("NM_002020",5) if ("NM_002020",6) exists.
//  
//      2) Realign inputs, in parallel, using supplied function realign:aln_t->alns_t
//
//      3) Keep top-scoring alignments per mrna-id.
//
//      4) Drop duplicates (same mrna-id, chr-id, chr-start, chr-stop).
//
//      5) Then select alignments for gene sharing a single common genomic-cds-start:
//          1) Drop alignments without a valid cds-start.
//          2) Prefer positions supported by more alignments.
//          3) Then prefer "NC_*" chr-id.
//          4) Then prefer lower chr-id.
//          5) Then prefer lower (more upstream) cds-start.
//
//      6) Sort by decreasing alignment score, increasing mrna-id.
//
// The above processing steps shall not make copies of aln_t.
//
// The aln_filter(...) below has:
//                      Number of if-statements: 0
//                              Number of loops: 0
//                   Control-flow nesting level: 0
//                      Direct use of iterators: 0
// Non-const variables (mutable state) declared: 0
//              Statements mutating local state: 0
//           Statements mutating external state: 0
//          Compile-time for the entire example: ~2.8s.
//
namespace example
{

struct aln_t
{
    using accession_t = std::string;
    using version_t   = uint32_t;
    using seq_id_t    = std::pair<accession_t, version_t>;
    using gene_id_t   = int32_t;
    using pos_t       = int64_t; // genomic position on chr.
                                 // signed; 1-based; 0 is invalid-pos;
                                 // x and -x refer to same nucleotide position
                                 // in forward and reverse orientations.
   
    static constexpr pos_t invalid_pos = 0;

    //-----------------------------------------------------------------------

      int64_t aln_id;
    gene_id_t gene_id;
     seq_id_t mrna_id;

     seq_id_t chr_id;
        pos_t chr_start;
        pos_t chr_stop;
        pos_t chr_cds_start_pos;

      int64_t score;

    struct alignment_details
    {
        //...
    };

    //---------------------------------------------------------------------------
    // Will make our type move-only to assert that our filtering
    // steps do not silently make copies under the hood.
    // (will fail to compile if it tries to)

               aln_t(const aln_t&) = delete;
    aln_t& operator=(const aln_t&) = delete;

                    aln_t(aln_t&&) = default;
         aln_t& operator=(aln_t&&) = default;

                           aln_t() = default;

};

using alns_t = std::vector<aln_t>;


namespace fn = rangeless::fn;
using fn::operators::operator%;   // arg % f % g % h; returns h(g(f(std::forward<Arg>(arg))));


//---------------------------------------------------------------------------
// Other similar libraries in c++ or other languages do not typically
// provide "filter to minimal (or maximal) elements" constructs, 
// (fn::where_min_by fn::where_max_by), or fn::group_all_by, fn::unique_all_by,
// or lazy transform_in_parallel.
//
// For demonstration we'll implement them below ourselves,
// and will then use my::group_all_by instead of fn::group_all_by, etc.
namespace my
{

static auto group_all_by = [](auto key_fn)
{
    return [key_fn = std::move(key_fn)](auto inputs)
    {
        return std::move(inputs)
      % fn::sort_by(key_fn)
      % fn::group_adjacent_by(key_fn);
    };
};

//---------------------------------------------------------------------------

static auto unique_all_by = [](auto key_fn)
{
    return [key_fn = std::move(key_fn)](auto inputs)
    {
        return std::move(inputs)
      % fn::sort_by(key_fn)
      % fn::unique_adjacent_by(key_fn);
    };
};

//---------------------------------------------------------------------------

static auto where_min_by = [](auto key_fn)
{
    return [key_fn = std::move(key_fn)](auto inputs)
    {
        // NB: implementation in fn:: is more involved to avoid sort/group.
        return std::move(inputs) 
      % fn::sort_by(key_fn)      // could use fn::lazy_sort_by here, because
                                 // we only need the first group below, but
                                 // lazy_sort_by is not stable.
      % fn::group_adjacent_by(key_fn)
      % fn::take_first(1)        // min-elements are in the first group
      % fn::concat();            // [[min-elements]] -> [min-elements]
    };
};

//---------------------------------------------------------------------------

static auto where_max_by = [](auto key_fn)
{   
    return my::where_min_by( fn::by::decreasing( std::move(key_fn)));
};

//---------------------------------------------------------------------------

static auto lazy_transform_in_parallel = [](auto fn, 
                                          size_t max_queue_size = std::thread::hardware_concurrency())
{
    assert(max_queue_size >= 1);

    return [max_queue_size, fn](auto inputs) // inputs can be an lazy InputRange
    {
        return std::move(inputs)

        //-------------------------------------------------------------------
        // Lazily yield std::async invocations of fn.

      % fn::transform([fn](auto inp)
        {
            return std::async( std::launch::async, 
                [inp = std::move(inp), fn]() mutable // mutable because inp will be moved-from
                {
                    return fn(std::move(inp));
                });
        })

        //-------------------------------------------------------------------
        // Cap the incoming sequence of tasks with a seq of `max_queue_size`-1
        // dummy future<...>'s, such that all real tasks make it 
        // from the other end of the sliding-window in the next stage.

      % fn::append( fn::seq([i = 1UL, max_queue_size]() mutable
        {
            using fn_out_t = decltype( fn( std::move( *inputs.begin())));
            return i++ < max_queue_size ? std::future<fn_out_t>() : fn::end_seq();
        }))

        //-------------------------------------------------------------------
        // Buffer executing async-tasks in a fixed-sized sliding window;
        // yield the result from the oldest (front) std::future.

      % fn::sliding_window(max_queue_size)

      % fn::transform([](auto view) // a view from a window? Get out!
        {
            return view.begin()->get();
        });
    };
};


// for demonstration: suppose a single invocation of transform-function is too small
// compared to async-invocation overhead, so we want to amortize the overhead by batching:
static auto batched_lazy_transform_in_parallel = [](auto fn,
                                                  size_t max_queue_size = std::thread::hardware_concurrency(),
                                                  size_t batch_size = 2)
{
    return [=](auto inputs)
    {
        return std::move(inputs)
      % fn::in_groups_of(batch_size)
      % my::lazy_transform_in_parallel( [&](auto inputs_batch)
        {
            return std::move(inputs_batch) 
                 % fn::transform(fn) 
                 % fn::to_vector(); // NB: since fn::transform is lazy,
                                    // we need to force eager-evalution
                                    // within this batch-transform function.
        }, max_queue_size)
      % fn::concat(); // flatten the batches of outputs
    };
};
                                                        

} //namespace my


//---------------------------------------------------------------------------

static alns_t realign(aln_t a) // realign stub: just return the original
{
    alns_t ret;
    ret.push_back(std::move(a));
    return ret;
}

#define LAMBDA(expr) ([&](const auto& _){ return expr; })


//---------------------------------------------------------------------------
// Filtering steps (5) and (6)

static auto filter_to_unique_cds_for_gene(alns_t alns_for_gene) -> alns_t
{
    return std::move(alns_for_gene)

    // (5.1) Keep alignments with valid cds-start.

  % fn::where LAMBDA( _.chr_cds_start_pos != aln_t::invalid_pos )

    //-------------------------------------------------------------------
    // (5.2) Keep alignments with most-ubiquitous valid cds-starts.

  % my::group_all_by LAMBDA( _.chr_cds_start_pos )
  % my::where_max_by LAMBDA( _.size() )
  % fn::concat()

    //-------------------------------------------------------------------
    // Filter to unique chr_cds_start_pos.
    // (5.3) Prefer on "NC_*" chr-accession,
    // (5.4) then lower chr-id, 
    // (5.5) then more upstream cds-start.

  % my::where_min_by LAMBDA( 
          std::make_tuple( _.chr_id.first.find("NC_") != 0,
                std::cref( _.chr_id ), 
                           _.chr_cds_start_pos))

#if 1
    //-------------------------------------------------------------------
    // (6) Sort by decreasing alignment score, then by increasing mrna-id.

  % fn::sort_by LAMBDA( 
          std::make_pair( fn::by::decreasing( _.score),
                                   std::cref( _.mrna_id) ))

#else // alternatively, e.g if you want to use your own sort

  % [](alns_t alns)
    {
        gfx::timsort(
            alns.begin(), alns.end(), 
            fn::by::make_comp([](const aln_t& a)
            {
                return std::make_pair(
                   fn::by::decreasing( a.score),
                            std::cref( a.mrna_id));
            });
        return std::move(alns);
    }
#endif
    ; // end of return-statement
}


//---------------------------------------------------------------------------

// Implement as lambda so can rely on the automatic return-type deduction,
// which will be some longwindedly-named lazy seq<...>

static auto aln_filter = [](auto alns_seq) // alns may be a lazy input-sequence (i.e. an InputRange)
{
    return std::move(alns_seq)

    //-----------------------------------------------------------------------
    // (1) Filter to latest mRNA-version per mRNA-accession
    
  % fn::group_adjacent_by( std::mem_fn( &aln_t::gene_id))
  % fn::transform( [](alns_t alns_for_gene) -> alns_t
    {
        return std::move(alns_for_gene)
      % my::group_all_by LAMBDA( std::cref( _.mrna_id.first))
      % fn::transform(
              my::where_max_by( std::mem_fn( &aln_t::mrna_id)))
      % fn::concat(); // un-group
    })
  % fn::concat()

    //-----------------------------------------------------------------------
    // (2) Realign in parallel

  % my::batched_lazy_transform_in_parallel(realign) // aln_t -> alns_t
  % fn::concat()
  
    //-----------------------------------------------------------------------
    // Per mrna-id:
    // (3) Keep top-scoring
    // (4) Drop duplicates
  
  % fn::group_adjacent_by( std::mem_fn( &aln_t::mrna_id)) // were made adjacent in (1)
  % fn::transform( [](alns_t alns_for_mrna) -> alns_t
    {
        return std::move(alns_for_mrna)
      % my::where_max_by( std::mem_fn( &aln_t::score))
      % my::unique_all_by LAMBDA( 
              std::tie( _.mrna_id, _.chr_id, _.chr_start, _.chr_stop ));
    })
  % fn::concat() 

    //-----------------------------------------------------------------------
    // (5), (6)

  % fn::group_adjacent_by( std::mem_fn( &aln_t::gene_id))
  % fn::transform( example::filter_to_unique_cds_for_gene)
  % fn::concat();
};

    // Curiously, group-by/transform/concat pattern appears to be 
    // very common. Perhaps it needs a separate abstraction?

}   // namespace example

//---------------------------------------------------------------------------

int main()
{
    using namespace example;
   
    alns_t alns{}; // normally these would come from a stream, but for the sake of example will yield from a vec.

    // GeneID:2
    alns.push_back(aln_t{ 101, 2, {"NM_000001", 2}, {"NC_000001", 1}, 1000000, 1001000, 100100, 100}); // keep.
    alns.push_back(aln_t{ 102, 2, {"NM_000001", 2}, {"NC_000001", 1}, 1000000, 1001000, 100100, 100}); // duplicate.
    alns.push_back(aln_t{ 103, 2, {"NM_000001", 2}, {"NC_000001", 1}, 1000001, 1001000, 100100, 50 }); // not top-scoring for this mrna.
    alns.push_back(aln_t{ 104, 2, {"NM_000001", 1}, {"NC_000001", 1}, 1000000, 1001000, 100100, 100}); // superceded mrna-version.
    alns.push_back(aln_t{ 201, 2, {"NM_000002", 1}, {"NC_000001", 1}, 1000000, 1001000, 0,      100}); // no valid-CDS.
    alns.push_back(aln_t{ 301, 2, {"NM_000003", 1}, {"NC_000001", 1}, 1000000, 1001000, 0,      100}); // no valid-CDS.
    alns.push_back(aln_t{ 401, 2, {"NM_000004", 1}, {"NC_000001", 1}, 1000000, 1001000, 0,      100}); // no valid-CDS.
    alns.push_back(aln_t{ 501, 2, {"NM_000005", 1}, {"NC_000001", 1}, 1000000, 1001000, 100100, 110}); // keep.
    alns.push_back(aln_t{ 801, 2, {"NM_000008", 1}, {"NC_000001", 1}, 1000000, 1001000, 100200, 100}); // not most-supported-CDS.

    // GeneID:3
    alns.push_back(aln_t{ 601, 3, {"NM_000005", 1}, {"NC_000001", 1}, 1000000, 1001000, 100100, 100});  // keep.
    alns.push_back(aln_t{ 701, 3, {"NM_000007", 1}, {"NT_000001", 1}, 1000000, 1001000, 100100, 100});  // not on NC.
  
    namespace fn = rangeless::fn; 
    using fn::operators::operator%;

    std::vector<int64_t> kept_ids{};

    // we could just std::move(alns) instead of fn::seq(...) here,
    // but demonstrating that input can also be a lazy seq, e.g. deserializing from an istream.
    fn::seq([&, i = 0UL]() mutable -> aln_t
    {
        return i < alns.size() ? std::move(alns[i++]) : fn::end_seq();
    })

  % example::aln_filter

  % fn::for_each( [&](aln_t a)
    {
        std::cerr << a.gene_id << "\t" << a.aln_id << "\n";
        kept_ids.push_back(a.aln_id);
    });

    assert((kept_ids == std::vector<int64_t>{{ 501, 101, 601 }} ));
            
    return 0;
}

