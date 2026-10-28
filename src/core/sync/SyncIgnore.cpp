#include "SyncIgnore.hpp"

#include "core/FsUtil.hpp"

namespace ucd::sync {
namespace {

// A glob, translated to a regular expression over a whole relative path.
QString globToRegex(const QString& glob) {
    QString out;
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const QChar c = glob.at(i);
        if (c == QLatin1Char('*')) {
            if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*')) {
                const bool slashAfter = i + 2 < glob.size() && glob.at(i + 2) == QLatin1Char('/');
                if (slashAfter) {
                    out += QStringLiteral("(?:.*/)?");  // "**/": any number of folders
                    i += 2;
                } else {
                    out += QStringLiteral(".*");  // "**" elsewhere: anything
                    i += 1;
                }
            } else {
                out += QStringLiteral("[^/]*");
            }
        } else if (c == QLatin1Char('?')) {
            out += QStringLiteral("[^/]");
        } else if (c == QLatin1Char('[')) {
            const qsizetype close = glob.indexOf(QLatin1Char(']'), i + 1);
            if (close < 0) {
                out += QStringLiteral("\\[");
                continue;
            }
            QString set = glob.mid(i + 1, close - i - 1);
            if (set.startsWith(QLatin1Char('!'))) set[0] = QLatin1Char('^');
            set.replace(QLatin1String("\\"), QLatin1String("\\\\"));
            out += QLatin1Char('[') + set + QLatin1Char(']');
            i = close;
        } else if (c == QLatin1Char('\\') && i + 1 < glob.size()) {
            out += QRegularExpression::escape(QString(glob.at(++i)));
        } else {
            out += QRegularExpression::escape(QString(c));
        }
    }
    return out;
}

}  // namespace

IgnoreRules::IgnoreRules(const QString& text) : text_(text) {
    int lineNo = 0;
    for (QString line : text.split(QLatin1Char('\n'))) {
        ++lineNo;
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        Rule rule;
        rule.line    = lineNo;
        rule.pattern = line;
        if (line.startsWith(QLatin1Char('!'))) {
            rule.negate = true;
            line        = line.mid(1);
        }
        if (line.endsWith(QLatin1Char('/'))) {
            rule.dirOnly = true;
            line.chop(1);
        }
        if (line.isEmpty()) {
            problems_ << Problem{lineNo, rule.pattern};
            continue;
        }
        // A slash before the end anchors the pattern at the top.
        const bool anchored = line.contains(QLatin1Char('/'));
        if (line.startsWith(QLatin1Char('/'))) line = line.mid(1);
        const QString body = globToRegex(line);
        QRegularExpression::PatternOptions options;
        if (kPathCase == Qt::CaseInsensitive) options |= QRegularExpression::CaseInsensitiveOption;
        rule.regex = QRegularExpression(QStringLiteral("^") + (anchored ? QString() : QStringLiteral("(?:.*/)?")) +
                                            body + QStringLiteral("$"),
                                        options);
        if (rule.regex.isValid()) rules_ << rule;
        else problems_ << Problem{lineNo, rule.pattern};
    }
}

const IgnoreRules::Rule* IgnoreRules::lastMatching(const QString& path, bool isDir) const {
    const Rule* last = nullptr;
    for (const Rule& rule : rules_) {
        if (rule.dirOnly && !isDir) continue;
        if (rule.regex.match(path).hasMatch()) last = &rule;
    }
    return last;
}

bool IgnoreRules::matchesItself(const QString& path, bool isDir) const {
    const Rule* last = lastMatching(path, isDir);
    return last && !last->negate;
}

IgnoreRules::Match IgnoreRules::explain(const QString& path, bool isDir) const {
    Match m;
    if (rules_.isEmpty()) return m;
    auto fill = [&](const Rule* rule, const QString& via) {
        m.line    = rule->line;
        m.pattern = rule->pattern;
        m.negate  = rule->negate;
        m.via     = via;
    };
    // A folder above it that is excluded decides, whatever the rules say
    // about the path itself.
    qsizetype from = 0;
    for (qsizetype slash; (slash = path.indexOf(QLatin1Char('/'), from)) >= 0; from = slash + 1) {
        const QString parent = path.left(slash);
        if (const Rule* rule = lastMatching(parent, true); rule && !rule->negate) {
            fill(rule, parent);
            return m;
        }
    }
    if (const Rule* rule = lastMatching(path, isDir)) fill(rule, {});
    return m;
}

bool IgnoreRules::matches(const QString& path, bool isDir) const {
    if (rules_.isEmpty()) return false;
    qsizetype from = 0;
    for (qsizetype slash; (slash = path.indexOf(QLatin1Char('/'), from)) >= 0; from = slash + 1)
        if (matchesItself(path.left(slash), true)) return true;
    return matchesItself(path, isDir);
}

QString exactPattern(const QString& path) {
    QString out = QStringLiteral("/");
    for (const QChar c : path) {
        if (c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('[') || c == QLatin1Char('\\') ||
            c == QLatin1Char('!') || c == QLatin1Char('#'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

}  // namespace ucd::sync
