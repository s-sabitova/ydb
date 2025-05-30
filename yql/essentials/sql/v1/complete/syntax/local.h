#pragma once

#include <yql/essentials/sql/v1/complete/sql_complete.h>

#include <yql/essentials/sql/v1/lexer/lexer.h>

#include <util/generic/string.h>
#include <util/generic/hash.h>
#include <util/generic/maybe.h>

namespace NSQLComplete {

    struct TLocalSyntaxContext {
        using TKeywords = THashMap<TString, TVector<TString>>;

        struct TPragma {
            TString Namespace;
        };

        struct TFunction {
            TString Namespace;
        };

        struct THint {
            EStatementKind StatementKind;
        };

        TKeywords Keywords;
        TMaybe<TPragma> Pragma;
        bool IsTypeName = false;
        TMaybe<TFunction> Function;
        TMaybe<THint> Hint;
    };

    class ILocalSyntaxAnalysis {
    public:
        using TPtr = THolder<ILocalSyntaxAnalysis>;

        virtual TLocalSyntaxContext Analyze(TCompletionInput input) = 0;
        virtual ~ILocalSyntaxAnalysis() = default;
    };

    ILocalSyntaxAnalysis::TPtr MakeLocalSyntaxAnalysis(TLexerSupplier lexer);

} // namespace NSQLComplete
