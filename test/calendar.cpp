#include <fn.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <iostream>

/*
[        D ](https://wiki.dlang.org/Component_programming_with_ranges)
[     Rust ](https://gist.github.com/DanielKeep/7c87e697d5810803d069)
[ range-v3 ](https://github.com/ericniebler/range-v3/blob/master/example/calendar.cpp)
[  Haskell ](https://github.com/BartoszMilewski/Calendar/blob/master/Main.hs)
*/
namespace greg  = boost::gregorian;
using date_t    = greg::date;
using dates_t   = std::vector<date_t>;

static void MakeCalendar(const uint16_t year, 
                         const  uint8_t num_months_horizontally, 
                          std::ostream& ostr)
{
    namespace fn = rangeless::fn;
    using fn::operators::operator%;

    fn::seq([year, date = date_t( year, greg::Jan, 1 )]() mutable
    {
        auto ret = date;
        date = date + greg::date_duration{ 1 };
        return ret.year() == year ? ret : fn::end_seq();
    })

  % fn::group_adjacent_by([](const date_t& d)
    {
        return std::make_pair(d.month(), d.week_number());
    })

    // format a line for a week, e.g. "       1  2  3  4  5"
  % fn::transform([&](dates_t wk_dates) -> std::pair<date_t::month_type, std::string>
    {
        const auto left_pad_amt =
            size_t(3 * ((wk_dates.front().day_of_week() + 7 - 1) % 7));

        return { wk_dates.front().month(),
                 wk_dates % fn::foldl(std::string(left_pad_amt, ' '),
                                   [](std::string ret_wk, const date_t& d)
                    {
                        return std::move(ret_wk)
                             + (d.day() < 10 ? "  " : " ")
                             + std::to_string(d.day());
                    }) };
    })

  % fn::group_adjacent_by(fn::by::first{}) // by month

  % fn::in_groups_of(num_months_horizontally)

  % fn::for_each([&](const auto& group) // group: vec<vec<(month, formatted-week-string)>>
    {
        static const std::array<std::string, 12> s_month_names{
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

        for(int row = -2; row < 6; row++) { // month-name + weekdays-header + up to 6 week-lines
            for(const auto& mo : group) {
                ostr << std::setiosflags(std::ios::left) 
                     << std::setw(25)
                     << (   row == -2        ?  "        " + s_month_names.at(mo.front().first - 1u)
                   :        row == -1        ?  " Mo Tu We Th Fr Sa Su"
                   : size_t(row) < mo.size() ?  mo[row].second // formatted week
                   :                            "                     ");
            }
            ostr << "\n";
        }
    });
}

int main()
{
    MakeCalendar(2019, 3, std::cout);
    return 0;
}

/*
>>time g++ -O2 -I../include -std=gnu++14 -o calendar.cpp.o -c calendar.cpp
real	0m2.777s
user	0m2.484s
sys	0m0.275s

Output:
         Jan                      Feb                      Mar
 Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su
           1  2  3  4                        1                        1
  5  6  7  8  9 10 11      2  3  4  5  6  7  8      2  3  4  5  6  7  8
 12 13 14 15 16 17 18      9 10 11 12 13 14 15      9 10 11 12 13 14 15
 19 20 21 22 23 24 25     16 17 18 19 20 21 22     16 17 18 19 20 21 22
 26 27 28 29 30 31        23 24 25 26 27 28        23 24 25 26 27 28 29
                                                   30 31

         Apr                      May                      Jun
 Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su
        1  2  3  4  5                  1  2  3      1  2  3  4  5  6  7
  6  7  8  9 10 11 12      4  5  6  7  8  9 10      8  9 10 11 12 13 14
 13 14 15 16 17 18 19     11 12 13 14 15 16 17     15 16 17 18 19 20 21
 20 21 22 23 24 25 26     18 19 20 21 22 23 24     22 23 24 25 26 27 28
 27 28 29 30              25 26 27 28 29 30 31     29 30

         Jul                      Aug                      Sep
 Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su
        1  2  3  4  5                     1  2         1  2  3  4  5  6
  6  7  8  9 10 11 12      3  4  5  6  7  8  9      7  8  9 10 11 12 13
 13 14 15 16 17 18 19     10 11 12 13 14 15 16     14 15 16 17 18 19 20
 20 21 22 23 24 25 26     17 18 19 20 21 22 23     21 22 23 24 25 26 27
 27 28 29 30 31           24 25 26 27 28 29 30     28 29 30
                          31

         Oct                      Nov                      Dec
 Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su     Mo Tu We Th Fr Sa Su
           1  2  3  4                        1         1  2  3  4  5  6
  5  6  7  8  9 10 11      2  3  4  5  6  7  8      7  8  9 10 11 12 13
 12 13 14 15 16 17 18      9 10 11 12 13 14 15     14 15 16 17 18 19 20
 19 20 21 22 23 24 25     16 17 18 19 20 21 22     21 22 23 24 25 26 27
 26 27 28 29 30 31        23 24 25 26 27 28 29     28 29 30 31
                          30

*/
