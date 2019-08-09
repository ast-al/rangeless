set -exuo pipefail
#../README.md has github-style local links; fix them into doxygen-style
cat ../README.md|perl -pae 's{test/calendar.cpp}{calendar_8cpp_source.html}; s{test/aln_filter.cpp}{aln__filter_8cpp_source.html}; s{https://ast-al.github.io/rangeless/html/}{}; s{https://\w+/staff/\w+/fn/html/}{}; s/\`\`\`cpp/\`\`\`{.cpp}/' > README.md
doxygen Doxyfile
rm README.md

ls $MY_WEB_DEST
cp -r html $MY_WEB_DEST/fn/
