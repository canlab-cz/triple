// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef LOGPRINTLAYOUT_H
#define LOGPRINTLAYOUT_H

/*
 * Page layout for printing the frame list (logprinter.cpp), Qt free so it
 * can be unit tested on the host (tests/gui/logprint_test.cpp).
 */

#include <algorithm>
#include <numeric>
#include <vector>

namespace logprint {

/*
 * Column widths that fit `available`. Columns keep their natural width
 * when everything fits; otherwise the widest columns are capped at a
 * common width (narrow ones such as bus or length stay intact) and their
 * text wraps. No column goes below `minWidth` unless even that cannot fit,
 * in which case all columns get an equal share.
 */
inline std::vector<int> fitColumns(const std::vector<int> &natural, int available, int minWidth)
{
    std::vector<int> w = natural;
    if (w.empty())
        return w;
    long sum = std::accumulate(w.begin(), w.end(), 0L);
    if (sum <= available)
        return w;
    int n = int(w.size());
    if (long(minWidth) * n >= available)
    {
        std::fill(w.begin(), w.end(), std::max(1, available / n));
        return w;
    }
    /* largest cap c with sum(min(w_i, c)) <= available */
    std::vector<int> sorted = natural;
    std::sort(sorted.begin(), sorted.end());
    long below = 0;                       /* sum of the columns narrower than the cap */
    int cap = minWidth;
    for (int i = 0; i < n; i++)
    {
        long rest = n - i;               /* columns that would be capped */
        long c = (available - below) / rest;
        if (c < sorted[size_t(i)])
        {
            cap = int(std::max<long>(c, minWidth));
            break;
        }
        below += sorted[size_t(i)];
        cap = sorted[size_t(i)];
    }
    for (int &x : w)
        x = std::max(std::min(x, cap), std::min(x, minWidth));
    return w;
}

/*
 * First row of every page. A row taller than a page gets a page of its own
 * (and is clipped when painted).
 */
inline std::vector<int> paginate(const std::vector<int> &rowHeights, int pageHeight)
{
    std::vector<int> starts;
    int used = 0;
    for (int i = 0; i < int(rowHeights.size()); i++)
    {
        if (starts.empty() || (used > 0 && used + rowHeights[size_t(i)] > pageHeight))
        {
            starts.push_back(i);
            used = 0;
        }
        used += rowHeights[size_t(i)];
    }
    if (starts.empty())
        starts.push_back(0);              /* an empty log still prints its header */
    return starts;
}

} // namespace logprint

#endif // LOGPRINTLAYOUT_H
