//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "optimizer.h"
#include "RemoveInactivePreprocessorBlocks.h"
#include "SmartRewriter.h"
#include "util.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Comment.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Parse/ParseAST.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/Tooling.h>


#include <cstdio>
#include <iostream>
#include <memory>
#include <stack>
#include <stdexcept>
#include <string>
#include <sstream>


using namespace clang;
using std::map;
using std::set;
using std::string;
using std::vector;


//#define CAIDE_DEBUG_MODE

#ifdef CAIDE_DEBUG_MODE

#define dbg(vals) std::cerr << vals
#define CAIDE_FUNC __FUNCTION__ << std::endl

#else

#define dbg(vals)
#define CAIDE_FUNC ""

#endif

namespace caide {
namespace internal {

// Contains information that DependenciesCollector passes to the next stage
struct SourceInfo {
    // key: Decl, value: what the key uses.
    map<Decl*, set<Decl*> > uses;

    // 'Roots of the dependency tree':
    // - int main()
    // - declarations marked with a comment '/// caide keep'
    set<Decl*> declsToKeep;

    // Delayed parsed functions.
    vector<FunctionDecl*> delayedParsedFunctions;

    // Declarations of static variables, grouped by their start location
    // (so comma separated declarations go into the same group).
    map<SourceLocation, vector<VarDecl*> > staticVariables;
};

class DependenciesCollector : public RecursiveASTVisitor<DependenciesCollector> {
private:
    SourceManager& sourceManager;
    SourceInfo& srcInfo;

    // There is no getParentDecl(stmt) function, so we maintain the stack of Decls,
    // with inner-most active Decl at the top of the stack
    std::stack<Decl*> declStack;

public:
    bool TraverseDecl(Decl* decl) {
        declStack.push(decl);
        bool ret = RecursiveASTVisitor<DependenciesCollector>::TraverseDecl(decl);
        declStack.pop();
        return ret;
    }

private:

    Decl* getCurrentDecl() {
        return declStack.empty() ? nullptr : declStack.top();
    }

    FunctionDecl* getCurrentFunction(Decl* decl) const {
        DeclContext* declCtx = decl->getLexicalDeclContext();
        return dyn_cast_or_null<FunctionDecl>(declCtx);
    }

    Decl* getParentDecl(Decl* decl) const {
        return dyn_cast_or_null<Decl>(decl->getLexicalDeclContext());
    }

    void insertReference(Decl* from, Decl* to) {
        if (!from || !to)
            return;
        // Multiple declarations of the same namespace must be distinguished:
        // it's possible that one of them should be deleted but no the other one.
        if (!isa<NamespaceDecl>(from))
            from = from->getCanonicalDecl();
        if (!isa<NamespaceDecl>(to))
            to = to->getCanonicalDecl();
        srcInfo.uses[from].insert(to);
        dbg("Reference   FROM    " << from->getDeclKindName() << " " << from
            << "<" << toString(sourceManager, from).substr(0, 30) << ">"
            << toString(sourceManager, from->getSourceRange())
            << "     TO     " << to->getDeclKindName() << " " << to
            << "<" << toString(sourceManager, to).substr(0, 30) << ">"
            << toString(sourceManager, to->getSourceRange())
            << std::endl);
    }

    void insertReferenceToType(Decl* from, const Type* to,
            set<const Type*>& seen)
    {
        if (!to)
            return;

        if (!seen.insert(to).second)
            return;

        if (const ElaboratedType* elaboratedType = dyn_cast<ElaboratedType>(to)) {
            insertReferenceToType(from, elaboratedType->getNamedType(), seen);
            return;
        }

        if (const ParenType* parenType = dyn_cast<ParenType>(to))
            insertReferenceToType(from, parenType->getInnerType(), seen);

        insertReference(from, to->getAsTagDecl());

        if (const ArrayType* arrayType = dyn_cast<ArrayType>(to))
            insertReferenceToType(from, arrayType->getElementType(), seen);

        if (const PointerType* pointerType = dyn_cast<PointerType>(to))
            insertReferenceToType(from, pointerType->getPointeeType(), seen);

        if (const ReferenceType* refType = dyn_cast<ReferenceType>(to))
            insertReferenceToType(from, refType->getPointeeType(), seen);

        if (const TypedefType* typedefType = dyn_cast<TypedefType>(to))
            insertReference(from, typedefType->getDecl());

        if (const CXXRecordDecl* recordDecl = to->getAsCXXRecordDecl()) {
            if ((recordDecl = recordDecl->getDefinition())) {
                bool isTemplated = recordDecl->getDescribedClassTemplate() != 0;
                TemplateSpecializationKind specKind = recordDecl->getTemplateSpecializationKind();
                if (isTemplated && (specKind == TSK_ImplicitInstantiation || specKind == TSK_Undeclared)) {}
                else {
                    for (const CXXBaseSpecifier* base = recordDecl->bases_begin();
                         base != recordDecl->bases_end(); ++base)
                    {
                        insertReferenceToType(from, base->getType(), seen);
                    }
                }
            }
        }

        if (const TemplateSpecializationType* tempSpecType =
                dyn_cast<TemplateSpecializationType>(to))
        {
            if (TemplateDecl* tempDecl = tempSpecType->getTemplateName().getAsTemplateDecl())
                insertReference(from, tempDecl);
            for (unsigned i = 0; i < tempSpecType->getNumArgs(); ++i) {
                const TemplateArgument& arg = tempSpecType->getArg(i);
                if (arg.getKind() == TemplateArgument::Type)
                    insertReferenceToType(from, arg.getAsType(), seen);
            }
        }
    }

    void insertReferenceToType(Decl* from, QualType to,
            set<const Type*>& seen)
    {
        insertReferenceToType(from, to.getTypePtrOrNull(), seen);
    }

    void insertReferenceToType(Decl* from, QualType to)
    {
        set<const Type*> seen;
        insertReferenceToType(from, to, seen);
    }

    void insertReferenceToType(Decl* from, const Type* to)
    {
        set<const Type*> seen;
        insertReferenceToType(from, to, seen);
    }

public:
    DependenciesCollector(SourceManager& srcMgr, SourceInfo& srcInfo_)
        : sourceManager(srcMgr)
        , srcInfo(srcInfo_)
    {
    }

    bool shouldVisitImplicitCode() const { return true; }
    bool shouldVisitTemplateInstantiations() const { return true; }
    bool shouldWalkTypesOfTypeLocs() const { return true; }

    bool VisitDecl(Decl* decl) {
        dbg("DECL " << decl->getDeclKindName() << " " << decl
            << "<" << toString(sourceManager, decl).substr(0, 30) << ">"
            << toString(sourceManager, getExpansionRange(sourceManager, decl))
            << std::endl);

        // Mark dependence on enclosing class/namespace.
        Decl* ctx = dyn_cast_or_null<Decl>(decl->getDeclContext());
        if (ctx && !isa<FunctionDecl>(ctx))
            insertReference(decl, ctx);

        if (!sourceManager.isInMainFile(decl->getLocStart()))
            return true;

        RawComment* comment = decl->getASTContext().getRawCommentForDeclNoCache(decl);
        if (!comment)
            return true;

        bool invalid = false;
        const char* beg = sourceManager.getCharacterData(comment->getLocStart(), &invalid);
        if (!beg || invalid)
            return true;

        const char* end =
            sourceManager.getCharacterData(comment->getLocEnd(), &invalid);
        if (!end || invalid)
            return true;

        static const string caideKeepComment = "caide keep";
        StringRef haystack(beg, end - beg + 1);
        StringRef needle(caideKeepComment);
        //std::cerr << toString(sourceManager, decl) << ": " << haystack.str() << std::endl;
        if (haystack.find(needle) != StringRef::npos)
            srcInfo.declsToKeep.insert(decl);

        return true;
    }

    bool VisitCallExpr(CallExpr* callExpr) {
        dbg(CAIDE_FUNC);
        Expr* callee = callExpr->getCallee();
        Decl* calleeDecl = callExpr->getCalleeDecl();

        if (!callee || !calleeDecl || isa<UnresolvedMemberExpr>(callee) || isa<CXXDependentScopeMemberExpr>(callee))
            return true;

        insertReference(getCurrentDecl(), calleeDecl);
        return true;
    }

    bool VisitCXXConstructExpr(CXXConstructExpr* constructorExpr) {
        dbg(CAIDE_FUNC);
        insertReference(getCurrentDecl(), constructorExpr->getConstructor());
        return true;
    }

    bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr* tempExpr) {
        if (TypeSourceInfo* tsi = tempExpr->getTypeSourceInfo())
            insertReferenceToType(getCurrentDecl(), tsi->getType());
        return true;
    }

    bool VisitTemplateTypeParmDecl(TemplateTypeParmDecl* paramDecl) {
        if (paramDecl->hasDefaultArgument())
            insertReferenceToType(getParentDecl(paramDecl), paramDecl->getDefaultArgument());
        return true;
    }

    bool VisitCXXNewExpr(CXXNewExpr* newExpr) {
        insertReferenceToType(getCurrentDecl(), newExpr->getAllocatedType());
        return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr* ref) {
        dbg(CAIDE_FUNC);
        Decl* parent = getCurrentDecl();
        insertReference(parent, ref->getDecl());
        NestedNameSpecifier* specifier = ref->getQualifier();
        while (specifier) {
            insertReferenceToType(parent, specifier->getAsType());
            specifier = specifier->getPrefix();
        }
        return true;
    }

    bool VisitCXXScalarValueInitExpr(CXXScalarValueInitExpr* initExpr) {
        if (TypeSourceInfo* tsi = initExpr->getTypeSourceInfo())
            insertReferenceToType(getCurrentDecl(), tsi->getType());
        return true;
    }

    bool VisitExplicitCastExpr(ExplicitCastExpr* castExpr) {
        insertReferenceToType(getCurrentDecl(), castExpr->getTypeAsWritten());
        return true;
    }

    bool VisitValueDecl(ValueDecl* valueDecl) {
        dbg(CAIDE_FUNC);
        // Mark any function as depending on its local variables.
        // TODO: detect unused local variables.
        insertReference(getCurrentFunction(valueDecl), valueDecl);

        insertReferenceToType(valueDecl, valueDecl->getType());
        return true;
    }

    bool VisitVarDecl(VarDecl* varDecl) {
        SourceLocation start = getExpansionStart(sourceManager, varDecl);
        if (!varDecl->isLocalVarDeclOrParm() && sourceManager.isInMainFile(start)) {
            srcInfo.staticVariables[start].push_back(varDecl);
            /*
            Technically, we cannot remove global static variables because
            their initializers may have side effects.
            The following code marks too many expressions as having side effects
            (e.g. it will mark an std::vector constructor as such):

            VarDecl* definition = varDecl->getDefinition();
            Expr* initExpr = definition ? definition->getInit() : nullptr;
            if (initExpr && initExpr->HasSideEffects(varDecl->getASTContext()))
                srcInfo.declsToKeep.insert(varDecl);

            The analysis of which functions *really* have side effects seems too
            complicated. So currently we simply remove unreferenced global static
            variables unless they are marked with a '/// caide keep' comment.
            */
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr* memberExpr) {
        dbg(CAIDE_FUNC);
        insertReference(getCurrentDecl(), memberExpr->getMemberDecl());
        return true;
    }

    bool VisitLambdaExpr(LambdaExpr* lambdaExpr) {
        dbg(CAIDE_FUNC);
        insertReference(getCurrentDecl(), lambdaExpr->getCallOperator());
        return true;
    }

    bool VisitFieldDecl(FieldDecl* field) {
        dbg(CAIDE_FUNC);
        insertReference(field, field->getParent());
        return true;
    }

    bool VisitTypedefNameDecl(TypedefNameDecl* typedefDecl) {
        dbg(CAIDE_FUNC);
        insertReferenceToType(typedefDecl, typedefDecl->getUnderlyingType());
        return true;
    }

    bool VisitTypeAliasDecl(TypeAliasDecl* aliasDecl) {
        dbg(CAIDE_FUNC);
        insertReference(aliasDecl, aliasDecl->getDescribedAliasTemplate());
        return true;
    }

    bool VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl* aliasTemplateDecl) {
        dbg(CAIDE_FUNC);
        insertReference(aliasTemplateDecl, aliasTemplateDecl->getInstantiatedFromMemberTemplate());
        return true;
    }

    bool VisitClassTemplateDecl(ClassTemplateDecl* templateDecl) {
        dbg(CAIDE_FUNC);
        insertReference(templateDecl, templateDecl->getTemplatedDecl());
        return true;
    }

    bool VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* specDecl) {
        dbg(CAIDE_FUNC);
        llvm::PointerUnion<ClassTemplateDecl*, ClassTemplatePartialSpecializationDecl*>
            instantiatedFrom = specDecl->getSpecializedTemplateOrPartial();

        if (instantiatedFrom.is<ClassTemplateDecl*>())
            insertReference(specDecl, instantiatedFrom.get<ClassTemplateDecl*>());
        else if (instantiatedFrom.is<ClassTemplatePartialSpecializationDecl*>())
            insertReference(specDecl, instantiatedFrom.get<ClassTemplatePartialSpecializationDecl*>());

        return true;
    }

    /*
    Every function template is represented as a FunctionTemplateDecl and a FunctionDecl
    (or something derived from FunctionDecl). The former contains template properties
    (such as the template parameter lists) while the latter contains the actual description
    of the template's contents. FunctionTemplateDecl::getTemplatedDecl() retrieves the
    FunctionDecl that describes the function template,
    FunctionDecl::getDescribedFunctionTemplate() retrieves the FunctionTemplateDecl
    from a FunctionDecl.

    We only use FunctionDecl's for dependency tracking.
     */
    bool VisitFunctionDecl(FunctionDecl* f) {
        dbg(CAIDE_FUNC);
        if (f->isMain())
            srcInfo.declsToKeep.insert(f);

        if (sourceManager.isInMainFile(f->getLocStart()) && f->isLateTemplateParsed())
            srcInfo.delayedParsedFunctions.push_back(f);

        if (f->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate) {
            // skip non-instantiated template function
            return true;
        }

        FunctionTemplateSpecializationInfo* specInfo = f->getTemplateSpecializationInfo();
        if (specInfo)
            insertReference(f, specInfo->getTemplate()->getTemplatedDecl());

        insertReferenceToType(f, f->getReturnType());

        insertReference(f, f->getInstantiatedFromMemberFunction());

        if (f->doesThisDeclarationHaveABody() &&
                sourceManager.isInMainFile(f->getLocStart()))
        {
            dbg("Moving to ";
                DeclarationName DeclName = f->getNameInfo().getName();
                string FuncName = DeclName.getAsString();
                std::cerr << FuncName << " at " <<
                    toString(sourceManager, f->getLocation()) << std::endl;
            );
        }

        return true;
    }

    bool VisitFunctionTemplateDecl(FunctionTemplateDecl* functionTemplate) {
        insertReference(functionTemplate,
                functionTemplate->getInstantiatedFromMemberTemplate());
        return true;
    }

    bool VisitCXXMethodDecl(CXXMethodDecl* method) {
        dbg(CAIDE_FUNC);
        insertReference(method, method->getParent());
        if (method->isVirtual()) {
            // Virtual methods may not be called directly. Assume that
            // if we need a class, we need all its virtual methods.
            // TODO: a more detailed analysis (walk the inheritance tree?)
            insertReference(method->getParent(), method);
        }
        return true;
    }

    bool VisitCXXRecordDecl(CXXRecordDecl* recordDecl) {
        insertReference(recordDecl, recordDecl->getDescribedClassTemplate());
        return true;
    }

    // sizeof, alignof
    bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr* expr) {
        if (expr->isArgumentType())
            insertReferenceToType(getCurrentDecl(), expr->getArgumentType());
        // if the argument is a variable it will be processed as DeclRefExpr
        return true;
    }
};


class UsageInfo {
private:
    SourceManager& sourceManager;
    SourceRangeComparer cmp;
    set<Decl*> usedDecls;
    set<SourceRange, SourceRangeComparer> locationsOfUsedDecls;

public:
    UsageInfo(SourceManager& sourceManager_, Rewriter& rewriter_)
        : sourceManager(sourceManager_)
    {
        cmp.cmp.rewriter = &rewriter_;
        locationsOfUsedDecls = set<SourceRange, SourceRangeComparer>(cmp);
    }

    bool isUsed(Decl* decl) const {
        if (usedDecls.find(decl) != usedDecls.end())
            return true;

        SourceRange range = getSourceRange(decl);
        return locationsOfUsedDecls.find(range) != locationsOfUsedDecls.end();
    }

    void addIfInMainFile(Decl* decl) {
        SourceRange range = getSourceRange(decl);

        if (sourceManager.isInMainFile(range.getBegin())) {
            dbg("USAGEINFO " <<
                decl->getDeclKindName() << " " << decl
                << "<" << toString(sourceManager, decl).substr(0, 30) << ">"
                << toString(sourceManager, range)
                << std::endl);
            usedDecls.insert(decl);
            locationsOfUsedDecls.insert(range);
        }
    }

private:
    SourceRange getSourceRange(Decl* decl) const {
        SourceRange range = getExpansionRange(sourceManager, decl);
        /*
        Decl* anotherDecl = nullptr;
        if (FunctionDecl* f = dyn_cast_or_null<FunctionDecl>(decl))
            anotherDecl = f->getDescribedFunctionTemplate();
        else if (FunctionTemplateDecl* ft = dyn_cast_or_null<FunctionTemplateDecl>(decl))
            anotherDecl = ft;

        if (anotherDecl) {
            SourceRange anotherRange = getExpansionRange(sourceManager, anotherDecl);
            if (sourceManager.isBeforeInTranslationUnit(anotherRange.getBegin(), range.getBegin()))
                range = anotherRange;
        }
        */
        return range;
    }
};

class OptimizerVisitor: public RecursiveASTVisitor<OptimizerVisitor> {
private:
    SourceManager& sourceManager;
    const UsageInfo usageInfo;
    set<Decl*> declared;
    set<NamespaceDecl*> usedNamespaces;
    SmartRewriter& rewriter;

    string toString(const Decl* decl) const {
        return internal::toString(sourceManager, decl);
    }

public:
    OptimizerVisitor(SourceManager& srcManager, const UsageInfo& usageInfo_, SmartRewriter& rewriter_)
        : sourceManager(srcManager)
        , usageInfo(usageInfo_)
        , rewriter(rewriter_)
    {}

    // When we remove code, we're only interested in the real code,
    // so no implicit instantiantions.
    bool shouldVisitImplicitCode() const { return false; }
    bool shouldVisitTemplateInstantiations() const { return false; }

    bool VisitEmptyDecl(EmptyDecl* decl) {
        if (sourceManager.isInMainFile(decl->getLocStart()))
            removeDecl(decl);
        return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl* namespaceDecl) {
        if (sourceManager.isInMainFile(namespaceDecl->getLocStart()) && !usageInfo.isUsed(namespaceDecl))
            removeDecl(namespaceDecl);
        return true;
    }

    /*
    bool VisitStmt(Stmt* stmt) {
        std::cerr << stmt->getStmtClassName() << endl;
        return true;
    }
    */

    /*
     Here's how template functions and classes are represented in the AST.
-FunctionTemplateDecl <-- the template
 |-TemplateTypeParmDecl
 |-FunctionDecl  <-- general (non-specialized) case
 |-FunctionDecl  <-- for each implicit instantiation of the template
 | `-CompoundStmt
 |   `-...
-FunctionDecl   <-- non-template or full explicit specialization of a template



|-ClassTemplateDecl <-- root template
| |-TemplateTypeParmDecl
| |-CXXRecordDecl  <-- non-specialized root template class
| | |-CXXRecordDecl
| | `-CXXMethodDecl...
| |-ClassTemplateSpecialization <-- non-instantiated explicit specialization (?)
| `-ClassTemplateSpecializationDecl <-- implicit instantiation of root template
|   |-TemplateArgument type 'double'
|   |-CXXRecordDecl
|   |-CXXMethodDecl...
|-ClassTemplatePartialSpecializationDecl <-- partial specialization
| |-TemplateArgument
| |-TemplateTypeParmDecl
| |-CXXRecordDecl
| `-CXXMethodDecl...
|-ClassTemplateSpecializationDecl <-- instantiation of explicit specialization
| |-TemplateArgument type 'int'
| |-CXXRecordDecl
| `-CXXMethodDecl...

     */

    bool needToRemoveFunction(FunctionDecl* functionDecl) const {
        if (functionDecl->isExplicitlyDefaulted() || functionDecl->isDeleted())
            return false;
        FunctionDecl* canonicalDecl = functionDecl->getCanonicalDecl();
        const bool funcIsUnused = !usageInfo.isUsed(canonicalDecl);
        const bool thisIsRedeclaration = !functionDecl->doesThisDeclarationHaveABody()
                && declared.find(canonicalDecl) != declared.end();
        return funcIsUnused || thisIsRedeclaration;
    }

    bool VisitFunctionDecl(FunctionDecl* functionDecl) {
        if (!sourceManager.isInMainFile(functionDecl->getLocStart()))
            return true;
        dbg(CAIDE_FUNC);

        // It may have been processed as FunctionTemplateDecl already
        // but we try it anyway.
        if (needToRemoveFunction(functionDecl))
            removeDecl(functionDecl);

        declared.insert(functionDecl->getCanonicalDecl());
        return true;
    }

    // TODO: dependencies on types of template parameters
    bool VisitFunctionTemplateDecl(FunctionTemplateDecl* templateDecl) {
        if (!sourceManager.isInMainFile(templateDecl->getLocStart()))
            return true;
        dbg(CAIDE_FUNC);
        FunctionDecl* functionDecl = templateDecl->getTemplatedDecl();

        // Correct source range may be given by either this template decl
        // or corresponding CXXMethodDecl, in case of template method of
        // template class. Choose the one that starts earlier.
        const bool processAsCXXMethod = sourceManager.isBeforeInTranslationUnit(
                getExpansionStart(sourceManager, functionDecl),
                getExpansionStart(sourceManager, templateDecl)
        );

        if (processAsCXXMethod) {
            // Will be processed as FunctionDecl later.
            return true;
        }

        if (needToRemoveFunction(functionDecl))
            removeDecl(templateDecl);
        return true;
    }

    bool VisitCXXRecordDecl(CXXRecordDecl* recordDecl) {
        if (!sourceManager.isInMainFile(recordDecl->getLocStart()))
            return true;
        bool isTemplated = recordDecl->getDescribedClassTemplate() != 0;
        TemplateSpecializationKind specKind = recordDecl->getTemplateSpecializationKind();
        if (isTemplated && (specKind == TSK_ImplicitInstantiation || specKind == TSK_Undeclared))
            return true;
        CXXRecordDecl* canonicalDecl = recordDecl->getCanonicalDecl();
        const bool classIsUnused = !usageInfo.isUsed(canonicalDecl);
        const bool thisIsRedeclaration = !recordDecl->isCompleteDefinition() && declared.find(canonicalDecl) != declared.end();

        if (classIsUnused || thisIsRedeclaration)
            removeDecl(recordDecl);

        declared.insert(canonicalDecl);
        return true;
    }

    bool VisitClassTemplateDecl(ClassTemplateDecl* templateDecl) {
        if (!sourceManager.isInMainFile(templateDecl->getLocStart()))
            return true;
        dbg(CAIDE_FUNC);
        ClassTemplateDecl* canonicalDecl = templateDecl->getCanonicalDecl();
        const bool classIsUnused = !usageInfo.isUsed(canonicalDecl);
        const bool thisIsRedeclaration = !templateDecl->isThisDeclarationADefinition() && declared.find(canonicalDecl) != declared.end();

        if (classIsUnused || thisIsRedeclaration)
            removeDecl(templateDecl);

        declared.insert(canonicalDecl);
        return true;
    }

    bool VisitTypedefDecl(TypedefDecl* typedefDecl) {
        if (!sourceManager.isInMainFile(typedefDecl->getLocStart()))
            return true;

        Decl* canonicalDecl = typedefDecl->getCanonicalDecl();
        if (!usageInfo.isUsed(canonicalDecl))
            removeDecl(typedefDecl);

        return true;
    }

    bool VisitTypeAliasDecl(TypeAliasDecl* aliasDecl) {
        if (!sourceManager.isInMainFile(aliasDecl->getLocStart()))
            return true;
        if (aliasDecl->getDescribedAliasTemplate()) {
            // This is a template alias; will be processed as TypeAliasTemplateDecl
            return true;
        }

        Decl* canonicalDecl = aliasDecl->getCanonicalDecl();
        if (!usageInfo.isUsed(canonicalDecl))
            removeDecl(aliasDecl);

        return true;
    }

    bool VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl* aliasDecl) {
        if (!sourceManager.isInMainFile(aliasDecl->getLocStart()))
            return true;
        if (!usageInfo.isUsed(aliasDecl))
            removeDecl(aliasDecl);
        return true;
    }

    bool VisitUsingDirectiveDecl(UsingDirectiveDecl* usingDecl) {
        if (!sourceManager.isInMainFile(usingDecl->getLocStart()))
            return true;
        NamespaceDecl* ns = usingDecl->getNominatedNamespace();
        if (ns && !usedNamespaces.insert(ns).second)
            removeDecl(usingDecl);
        return true;
    }

private:
    void removeDecl(Decl* decl) {
        if (!decl)
            return;
        SourceLocation start = getExpansionStart(sourceManager, decl);
        SourceLocation end = getExpansionEnd(sourceManager, decl);

        SourceLocation semicolonAfterDefinition = findSemiAfterLocation(end, decl->getASTContext());
        dbg("REMOVE " << decl->getDeclKindName() << " "
            << decl << ": " << toString(start) << " " << toString(end)
            << " ; " << toString(semicolonAfterDefinition) << std::endl);
        if (semicolonAfterDefinition.isValid())
            end = semicolonAfterDefinition;
        Rewriter::RewriteOptions opts;
        opts.RemoveLineIfEmpty = true;
        rewriter.removeRange(SourceRange(start, end), opts);

        RawComment* comment = decl->getASTContext().getRawCommentForDeclNoCache(decl);
        if (comment)
            rewriter.removeRange(comment->getSourceRange(), opts);
    }

    string toString(const SourceLocation& loc) const {
        return loc.printToString(sourceManager);
    }
};

class OptimizerConsumer: public ASTConsumer {
public:
    explicit OptimizerConsumer(CompilerInstance& compiler_, SmartRewriter& smartRewriter_,
                Rewriter& rewriter_, RemoveInactivePreprocessorBlocks& ppCallbacks_,
                string& result_)
        : compiler(compiler_)
        , sourceManager(compiler.getSourceManager())
        , smartRewriter(smartRewriter_)
        , rewriter(rewriter_)
        , ppCallbacks(ppCallbacks_)
        , result(result_)
    {}

    virtual void HandleTranslationUnit(ASTContext& Ctx) override {
        //std::cerr << "Build dependency graph" << std::endl;
        DependenciesCollector depsVisitor(sourceManager, srcInfo);
        depsVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());

        // Source range of delayed-parsed template functions includes only declaration part.
        //     Force their parsing to get correct source ranges.
        //     Suppress error messages temporarily (it's OK for these functions
        //     to be malformed).
        clang::Sema& sema = compiler.getSema();
        sema.getDiagnostics().setSuppressAllDiagnostics(true);
        for (FunctionDecl* f : srcInfo.delayedParsedFunctions) {
            clang::LateParsedTemplate* lpt = sema.LateParsedTemplateMap[f];
            sema.LateTemplateParser(sema.OpaqueParser, *lpt);
        }
        sema.getDiagnostics().setSuppressAllDiagnostics(false);

        //std::cerr << "Search for used decls" << std::endl;
        UsageInfo usageInfo(sourceManager, rewriter);
        set<Decl*> used;
        set<Decl*> queue;
        for (Decl* decl : srcInfo.declsToKeep)
            queue.insert(isa<NamespaceDecl>(decl) ? decl : decl->getCanonicalDecl());

        while (!queue.empty()) {
            Decl* decl = *queue.begin();
            queue.erase(queue.begin());
            if (used.insert(decl).second) {
                queue.insert(srcInfo.uses[decl].begin(), srcInfo.uses[decl].end());
                usageInfo.addIfInMainFile(decl);

                if (CXXRecordDecl* record = dyn_cast<CXXRecordDecl>(decl)) {
                    // No implicit calls to destructors in AST; assume that
                    // if a class is used, its destructor is used too.
                    if (CXXDestructorDecl* destructor = record->getDestructor())
                        queue.insert(destructor);
                }
            }
        }

        used.clear();

        //std::cerr << "Remove unused decls" << std::endl;
        OptimizerVisitor visitor(sourceManager, usageInfo, smartRewriter);
        visitor.TraverseDecl(Ctx.getTranslationUnitDecl());

        removeUnusedVariables(usageInfo, Ctx);

        ppCallbacks.Finalize();

        smartRewriter.applyChanges();

        //std::cerr << "Done!" << std::endl;
        result = getResult();
    }

private:
    // Variables are a special case because there may be many
    // comma separated variables in one definition.
    void removeUnusedVariables(const UsageInfo& usageInfo, ASTContext& ctx) {
        Rewriter::RewriteOptions opts;
        opts.RemoveLineIfEmpty = true;

        for (const auto& kv : srcInfo.staticVariables) {
            const vector<VarDecl*>& vars = kv.second;
            const size_t n = vars.size();
            vector<bool> isUsed(n, true);
            size_t lastUsed = n;
            for (size_t i = 0; i < n; ++i) {
                VarDecl* var = vars[i];
                isUsed[i] = usageInfo.isUsed(var->getCanonicalDecl());
                if (isUsed[i])
                    lastUsed = i;
            }

            SourceLocation startOfType = kv.first;
            SourceLocation endOfLastVar = getExpansionEnd(sourceManager, vars.back());

            if (lastUsed == n) {
                // all variables are unused
                SourceLocation semiColon = findSemiAfterLocation(endOfLastVar, ctx);
                SourceRange range(startOfType, semiColon);
                smartRewriter.removeRange(range, opts);
            } else {
                for (size_t i = 0; i < lastUsed; ++i) if (!isUsed[i]) {
                    // beginning of variable name
                    SourceLocation beg = vars[i]->getLocation();

                    // end of initializer
                    SourceLocation end = getExpansionEnd(sourceManager, vars[i]);

                    if (i+1 < n) {
                        // comma
                        end = findTokenAfterLocation(end, ctx, tok::comma);
                    }

                    if (beg.isValid() && end.isValid()) {
                        SourceRange range(beg, end);
                        smartRewriter.removeRange(range, opts);
                    }
                }
                if (lastUsed + 1 != n) {
                    // clear all remaining variables, starting with comma
                    SourceLocation end = getExpansionEnd(sourceManager, vars[lastUsed]);
                    SourceLocation comma = findTokenAfterLocation(end, ctx, tok::comma);
                    SourceRange range(comma, endOfLastVar);
                    smartRewriter.removeRange(range, opts);
                }
            }
        }
    }

    string getResult() const {
        // At this point the rewriter's buffer should be full with the rewritten
        // file contents.
        if (const RewriteBuffer* rewriteBuf =
                smartRewriter.getRewriteBufferFor(sourceManager.getMainFileID()))
            return string(rewriteBuf->begin(), rewriteBuf->end());

        // No changes
        bool invalid;
        const llvm::MemoryBuffer* buf = sourceManager.getBuffer(sourceManager.getMainFileID(), &invalid);
        if (buf && !invalid)
            return string(buf->getBufferStart(), buf->getBufferEnd());
        else
            return "Inliner error"; // something's wrong
    }

private:
    CompilerInstance& compiler;
    SourceManager& sourceManager;
    SmartRewriter& smartRewriter;
    Rewriter& rewriter;
    RemoveInactivePreprocessorBlocks& ppCallbacks;
    string& result;
    SourceInfo srcInfo;
};


class OptimizerFrontendAction : public ASTFrontendAction {
private:
    Rewriter& rewriter;
    SmartRewriter& smartRewriter;
    string& result;
    const set<string>& macrosToKeep;
public:
    OptimizerFrontendAction(Rewriter& rewriter_, SmartRewriter& smartRewriter_,
           string& result_, const set<string>& macrosToKeep_)
        : rewriter(rewriter_)
        , smartRewriter(smartRewriter_)
        , result(result_)
        , macrosToKeep(macrosToKeep_)
    {}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, StringRef /*file*/) override
    {
        if (!compiler.hasSourceManager())
            throw "No source manager";
        rewriter.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());
        auto ppCallbacks = std::unique_ptr<RemoveInactivePreprocessorBlocks>(
            new RemoveInactivePreprocessorBlocks(compiler.getSourceManager(), smartRewriter, macrosToKeep));
        auto consumer = std::unique_ptr<OptimizerConsumer>(new OptimizerConsumer(compiler, smartRewriter, rewriter, *ppCallbacks, result));
        compiler.getPreprocessor().addPPCallbacks(std::move(ppCallbacks));
        return std::move(consumer);
    }
};

class OptimizerFrontendActionFactory: public tooling::FrontendActionFactory {
private:
    Rewriter& rewriter;
    SmartRewriter smartRewriter;
    string& result;
    const set<string>& macrosToKeep;
public:
    OptimizerFrontendActionFactory(Rewriter& rewriter_, string& result_,
                const set<string>& macrosToKeep_)
        : rewriter(rewriter_)
        , smartRewriter(rewriter_)
        , result(result_)
        , macrosToKeep(macrosToKeep_)
    {}
    FrontendAction* create() {
        return new OptimizerFrontendAction(rewriter, smartRewriter, result, macrosToKeep);
    }
};



Optimizer::Optimizer(const vector<string>& cmdLineOptions_,
                     const vector<string>& macrosToKeep_)
    : cmdLineOptions(cmdLineOptions_)
    , macrosToKeep(macrosToKeep_.begin(), macrosToKeep_.end())
{}

string Optimizer::doOptimize(const string& cppFile) {
    std::unique_ptr<tooling::FixedCompilationDatabase> compilationDatabase(
        createCompilationDatabaseFromCommandLine(cmdLineOptions));

    vector<string> sources;
    sources.push_back(cppFile);

    clang::tooling::ClangTool tool(*compilationDatabase, sources);

    Rewriter rewriter;
    string result;
    OptimizerFrontendActionFactory factory(rewriter, result, macrosToKeep);

    int ret = tool.run(&factory);
    if (ret != 0)
        throw std::runtime_error("Compilation error");

    return result;
}

}
}

