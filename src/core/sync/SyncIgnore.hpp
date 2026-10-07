#pragma once
// .gitignore-style patterns, for .syncignore (shared by every member) and
// .syncskip (this device only):
//
//   # comment           lines starting with # are skipped
//   *.tmp               a name anywhere in the tree; * and ? stay within one
//                       name, [abc] is a set of characters
//   /build              anchored: only at the top of the synced folder
//   docs/*.pdf          a pattern with a slash is anchored too
//   cache/              a trailing slash matches folders only
//   **/logs, a/**/b     ** spans any number of folders
//   !keep.tmp           a later ! pattern brings a match back
//
// As in git, nothing inside an excluded folder can be brought back.

#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace ucd::sync {

class IgnoreRules {
public:
    IgnoreRules() = default;
    explicit IgnoreRules(const QString& text);

    // Whether `path` (relative, '/'-separated) or a folder above it matches.
    bool matches(const QString& path, bool isDir) const;
    bool isEmpty() const { return rules_.isEmpty(); }
    const QString& text() const { return text_; }

private:
    struct Rule {
        QRegularExpression regex;
        bool               negate  = false;
        bool               dirOnly = false;
    };
    bool matchesItself(const QString& path, bool isDir) const;

    QVector<Rule> rules_;
    QString       text_;
};

// A pattern matching exactly `path`, for adding one file to .syncskip.
QString exactPattern(const QString& path);

}  // namespace ucd::sync
