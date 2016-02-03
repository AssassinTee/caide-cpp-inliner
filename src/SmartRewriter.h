//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <set>
#include <vector>

#include "clang/Rewrite/Core/Rewriter.h"


struct RewriteItem {
    clang::SourceRange range;
    clang::Rewriter::RewriteOptions opts;
};

struct SourceLocationComparer {
    bool operator() (const clang::SourceLocation& lhs, const clang::SourceLocation& rhs) const;
    clang::Rewriter* rewriter;
};

struct SourceRangeComparer {
    bool operator() (const clang::SourceRange& lhs, const clang::SourceRange& rhs) const;
    SourceLocationComparer cmp;
};

struct RewriteItemComparer {
    bool operator() (const RewriteItem& lhs, const RewriteItem& rhs) const;
    SourceLocationComparer cmp;
};


class SmartRewriter {
public:
    explicit SmartRewriter(clang::Rewriter& _rewriter);

    bool canRemoveRange(const clang::SourceRange& range) const;
    bool removeRange(const clang::SourceRange& range, clang::Rewriter::RewriteOptions opts);
    const clang::RewriteBuffer* getRewriteBufferFor(clang::FileID fileID) const;
    void applyChanges();

private:
    clang::Rewriter& rewriter;
    std::set<RewriteItem, RewriteItemComparer> removed;
    RewriteItemComparer comparer;
    bool changesApplied;
};

