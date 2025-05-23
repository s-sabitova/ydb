#include "yql_highlighter.h"

#include "yql_position.h"

#include <yql/essentials/public/issue/yql_issue.h>
#include <yql/essentials/sql/settings/translation_settings.h>
#include <yql/essentials/sql/v1/lexer/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4_ansi/lexer.h>

#include <util/charset/utf8.h>
#include <util/string/strip.h>

#include <regex>

namespace NYdb::NConsoleClient {
    using NSQLTranslation::ILexer;
    using NSQLTranslation::ParseTranslationSettings;
    using NSQLTranslation::SQL_MAX_PARSER_ERRORS;
    using NSQLTranslation::TParsedToken;
    using NSQLTranslation::TParsedTokenList;
    using NSQLTranslation::TTranslationSettings;
    using NSQLTranslationV1::IsProbablyKeyword;
    using NSQLTranslationV1::MakeLexer;
    using NYql::TIssues;

    using std::regex_constants::ECMAScript;
    using std::regex_constants::icase;

    NSQLTranslationV1::TLexers MakeLexers() {
        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
        lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
        return lexers;
    }

    constexpr const char* builtinFunctionPattern = ( //
        "^("
        "abs|aggregate_by|aggregate_list|aggregate_list_distinct|agg_list|agg_list_distinct|"
        "as_table|avg|avg_if|adaptivedistancehistogram|adaptivewardhistogram|adaptiveweighthistogram|"
        "addmember|addtimezone|aggregateflatten|aggregatetransforminput|aggregatetransformoutput|"
        "aggregationfactory|asatom|asdict|asdictstrict|asenum|aslist|asliststrict|asset|assetstrict|"
        "asstruct|astagged|astuple|asvariant|atomcode|bitcast|bit_and|bit_or|bit_xor|bool_and|"
        "bool_or|bool_xor|bottom|bottom_by|blockwardhistogram|blockweighthistogram|cast|coalesce|"
        "concat|concat_strict|correlation|count|count_if|covariance|covariance_population|"
        "covariance_sample|callableargument|callableargumenttype|callableresulttype|callabletype|"
        "callabletypecomponents|callabletypehandle|choosemembers|combinemembers|countdistinctestimate|"
        "currentauthenticateduser|currentoperationid|currentoperationsharedid|currenttzdate|"
        "currenttzdatetime|currenttztimestamp|currentutcdate|currentutcdatetime|currentutctimestamp|"
        "dense_rank|datatype|datatypecomponents|datatypehandle|dictaggregate|dictcontains|dictcreate|"
        "dicthasitems|dictitems|dictkeytype|dictkeys|dictlength|dictlookup|dictpayloadtype|dictpayloads|"
        "dicttype|dicttypecomponents|dicttypehandle|each|each_strict|emptydicttype|emptydicttypehandle|"
        "emptylisttype|emptylisttypehandle|endswith|ensure|ensureconvertibleto|ensuretype|enum|"
        "evaluateatom|evaluatecode|evaluateexpr|evaluatetype|expandstruct|filter|filter_strict|find|"
        "first_value|folder|filecontent|filepath|flattenmembers|forceremovemember|forceremovemembers|"
        "forcerenamemembers|forcespreadmembers|formatcode|formattype|frombytes|frompg|funccode|"
        "greatest|grouping|gathermembers|generictype|histogram|hll|hoppingwindowpgcast|hyperloglog|"
        "if|if_strict|instanceof|json_exists|json_query|json_value|jointablerow|just|lag|last_value|"
        "lead|least|len|length|like|likely|like_strict|lambdaargumentscount|lambdacode|"
        "lambdaoptionalargumentscount|linearhistogram|listaggregate|listall|listany|listavg|listcode|"
        "listcollect|listconcat|listcreate|listdistinct|listenumerate|listextend|listextendstrict|"
        "listextract|listfilter|listflatmap|listflatten|listfold|listfold1|listfold1map|listfoldmap|"
        "listfromrange|listfromtuple|listhas|listhasitems|listhead|listindexof|listitemtype|listlast|"
        "listlength|listmap|listmax|listmin|listnotnull|listreplicate|listreverse|listskip|"
        "listskipwhile|listskipwhileinclusive|listsort|listsortasc|listsortdesc|listsum|listtake|"
        "listtakewhile|listtakewhileinclusive|listtotuple|listtype|listtypehandle|listunionall|"
        "listuniq|listzip|listzipall|loghistogram|logarithmichistogram|max|max_by|max_of|median|"
        "min|min_by|min_of|mode|multi_aggregate_by|nanvl|nvl|nothing|nulltype|nulltypehandle|"
        "optionalitemtype|optionaltype|optionaltypehandle|percentile|parsefile|parsetype|"
        "parsetypehandle|pgand|pgarray|pgcall|pgconst|pgnot|pgop|pgor|pickle|quotecode|range|"
        "range_strict|rank|regexp|regexp_strict|rfind|row_number|random|randomnumber|randomuuid|"
        "removemember|removemembers|removetimezone|renamemembers|replacemember|reprcode|resourcetype|"
        "resourcetypehandle|resourcetypetag|some|stddev|stddev_population|stddev_sample|substring|"
        "sum|sum_if|sessionstart|sessionwindow|setcreate|setdifference|setincludes|setintersection|"
        "setisdisjoint|setsymmetricdifference|setunion|spreadmembers|stablepickle|startswith|staticmap|"
        "staticzip|streamitemtype|streamtype|streamtypehandle|structmembertype|structmembers|"
        "structtypecomponents|structtypehandle|subqueryextend|subqueryextendfor|subquerymerge|"
        "subquerymergefor|subqueryunionall|subqueryunionallfor|subqueryunionmerge|subqueryunionmergefor"
        "|top|topfreq|top_by|tablename|tablepath|tablerecordindex|tablerow|tablerows|taggedtype|"
        "taggedtypecomponents|taggedtypehandle|tobytes|todict|tomultidict|topg|toset|tosorteddict|"
        "tosortedmultidict|trymember|tupleelementtype|tupletype|tupletypecomponents|tupletypehandle|"
        "typehandle|typekind|typeof|udaf|udf|unittype|unpickle|untag|unwrap|variance|variance_population|"
        "variance_sample|variant|varianttype|varianttypehandle|variantunderlyingtype|voidtype|"
        "voidtypehandle|way|worldcode|weakfield"
        ")$");

    constexpr const char* typePattern = ( //
        "^("
        "bool|date|datetime|decimal|double|float|int16|int32|int64|int8|interval|json|jsondocument|"
        "string|timestamp|tzdate|tzdatetime|tztimestamp|uint16|uint32|uint64|uint8|utf8|uuid|yson|"
        "text|bytes"
        ")$");

    bool IsAnsiQuery(const TString& queryUtf8) {
        TTranslationSettings settings;
        TIssues issues;
        ParseTranslationSettings(queryUtf8, settings, issues);
        return settings.AnsiLexer;
    }

    class TYQLHighlighter: public IYQLHighlighter {
    public:
        explicit TYQLHighlighter(TColorSchema color)
            : Coloring(color)
            , BuiltinFunctionRegex(builtinFunctionPattern, ECMAScript | icase)
            , TypeRegex(typePattern, ECMAScript | icase)
            , CppLexer(MakeLexer(MakeLexers(), /* ansi = */ false, /* antlr4 = */ true))
            , ANSILexer(MakeLexer(MakeLexers(), /* ansi = */ true, /* antlr4 = */ true))
        {
        }

    public:
        void Apply(TStringBuf queryUtf8, TColors& colors) {
            Tokens = Tokenize(TString(queryUtf8));
            auto mapping = TYQLPositionMapping::Build(TString(queryUtf8));

            for (std::size_t i = 0; i < Tokens.size() - 1; ++i) {
                const auto& token = Tokens.at(i);
                const auto color = ColorOf(token, i);

                const std::ptrdiff_t start = mapping.RawPos(token);
                const std::ptrdiff_t length = GetNumberOfUTF8Chars(token.Content);
                const std::ptrdiff_t stop = start + length;

                std::fill(std::next(std::begin(colors), start),
                          std::next(std::begin(colors), stop), color);
            }
        }

    private:
        TParsedTokenList Tokenize(const TString& queryUtf8) {
            ILexer& lexer = *SuitableLexer(queryUtf8);

            TParsedTokenList tokens;
            TIssues issues;
            NSQLTranslation::Tokenize(lexer, queryUtf8, "Query", tokens, issues, SQL_MAX_PARSER_ERRORS);
            return tokens;
        }

        ILexer::TPtr& SuitableLexer(const TString& queryUtf8) {
            if (IsAnsiQuery(queryUtf8)) {
                return ANSILexer;
            }
            return CppLexer;
        }

        TColor ColorOf(const TParsedToken& token, size_t index) {
            if (IsString(token)) {
                return Coloring.string;
            }
            if (IsFunctionIdentifier(token, index)) {
                return Coloring.identifier.function;
            }
            if (IsTypeIdentifier(token)) {
                return Coloring.identifier.type;
            }
            if (IsVariableIdentifier(token)) {
                return Coloring.identifier.variable;
            }
            if (IsQuotedIdentifier(token)) {
                return Coloring.identifier.quoted;
            }
            if (IsNumber(token)) {
                return Coloring.number;
            }
            if (IsOperation(token)) {
                return Coloring.operation;
            }
            if (IsKeyword(token)) {
                return Coloring.keyword;
            }
            if (IsComment(token)) {
                return Coloring.comment;
            }
            return Coloring.unknown;
        }

        bool IsKeyword(const TParsedToken& token) const {
            return IsProbablyKeyword(token);
        }

        bool IsOperation(const TParsedToken& token) const {
            return (
                token.Name == "EQUALS" ||
                token.Name == "EQUALS2" ||
                token.Name == "NOT_EQUALS" ||
                token.Name == "NOT_EQUALS2" ||
                token.Name == "LESS" ||
                token.Name == "LESS_OR_EQ" ||
                token.Name == "GREATER" ||
                token.Name == "GREATER_OR_EQ" ||
                token.Name == "SHIFT_LEFT" ||
                token.Name == "ROT_LEFT" ||
                token.Name == "AMPERSAND" ||
                token.Name == "PIPE" ||
                token.Name == "DOUBLE_PIPE" ||
                token.Name == "STRUCT_OPEN" ||
                token.Name == "STRUCT_CLOSE" ||
                token.Name == "PLUS" ||
                token.Name == "MINUS" ||
                token.Name == "TILDA" ||
                token.Name == "ASTERISK" ||
                token.Name == "SLASH" ||
                token.Name == "PERCENT" ||
                token.Name == "SEMICOLON" ||
                token.Name == "DOT" ||
                token.Name == "COMMA" ||
                token.Name == "LPAREN" ||
                token.Name == "RPAREN" ||
                token.Name == "QUESTION" ||
                token.Name == "COLON" ||
                token.Name == "COMMAT" ||
                token.Name == "DOUBLE_COMMAT" ||
                token.Name == "DOLLAR" ||
                token.Name == "LBRACE_CURLY" ||
                token.Name == "RBRACE_CURLY" ||
                token.Name == "CARET" ||
                token.Name == "NAMESPACE" ||
                token.Name == "ARROW" ||
                token.Name == "RBRACE_SQUARE" ||
                token.Name == "LBRACE_SQUARE");
        }

        bool IsFunctionIdentifier(const TParsedToken& token, size_t index) {
            if (token.Name != "ID_PLAIN") {
                return false;
            }
            return std::regex_search(token.Content.begin(), token.Content.end(), BuiltinFunctionRegex) ||
                   (2 <= index && Tokens.at(index - 1).Name == "NAMESPACE" && Tokens.at(index - 2).Name == "ID_PLAIN") ||
                   (index < Tokens.size() - 1 && Tokens.at(index + 1).Name == "NAMESPACE");
        }

        bool IsTypeIdentifier(const TParsedToken& token) const {
            return token.Name == "ID_PLAIN" && std::regex_search(token.Content.begin(), token.Content.end(), TypeRegex);
        }

        bool IsVariableIdentifier(const TParsedToken& token) const {
            return token.Name == "ID_PLAIN";
        }

        bool IsQuotedIdentifier(const TParsedToken& token) const {
            return token.Name == "ID_QUOTED";
        }

        bool IsString(const TParsedToken& token) const {
            return token.Name == "STRING_VALUE";
        }

        bool IsNumber(const TParsedToken& token) const {
            return token.Name == "DIGITS" ||
                   token.Name == "INTEGER_VALUE" ||
                   token.Name == "REAL" ||
                   token.Name == "BLOB";
        }

        bool IsComment(const TParsedToken& token) const {
            return token.Name == "COMMENT";
        }

    private:
        TColorSchema Coloring;
        std::regex BuiltinFunctionRegex;
        std::regex TypeRegex;

        ILexer::TPtr CppLexer;
        ILexer::TPtr ANSILexer;

        TParsedTokenList Tokens;
    };

    IYQLHighlighter::TPtr MakeYQLHighlighter(TColorSchema color) {
        return IYQLHighlighter::TPtr(new TYQLHighlighter(std::move(color)));
    }

} // namespace NYdb::NConsoleClient
