/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#ifndef LO_CLANG_SHARED_PLUGINS

#include <cassert>
#include <string>
#include <iostream>
#include <fstream>
#include <set>

#include <clang/AST/CXXInheritance.h>

#include "check.hxx"
#include "plugin.hxx"

/**
 * Look for:
 *     &(*x)
 * when x is a smart pointer like unique_ptr. That can be transformed to:
 *     x.get()
 *
 * This is important when x is null, because in that case *x is undefined behavior. This can lead to
 * difficult bugs because the compiler will often let you get away with it, but some compilers might
 * also take advantage of that to assume that x is never null and then optimise away chunks of code.
 * See tdf#173585 for an example.
 *
 * Either way x.get() is easier to understand.
 */

namespace
{
class SmartPointerDerefAddress : public loplugin::FilteringPlugin<SmartPointerDerefAddress>
{
public:
    explicit SmartPointerDerefAddress(loplugin::InstantiationData const& data)
        : FilteringPlugin(data)
    {
    }

    virtual void run() override
    {
        if (preRun())
            TraverseDecl(compiler.getASTContext().getTranslationUnitDecl());
    }

    bool VisitUnaryOperator(UnaryOperator const*);
};

bool SmartPointerDerefAddress::VisitUnaryOperator(UnaryOperator const* unaryOperator)
{
    if (ignoreLocation(unaryOperator))
        return true;
    if (unaryOperator->getBeginLoc().isMacroID())
        return true;
    if (unaryOperator->getOpcode() != UO_AddrOf)
        return true;
    auto subExpr = unaryOperator->getSubExpr()->IgnoreParenImpCasts();
    auto innerOp = dyn_cast<CXXOperatorCallExpr>(subExpr);
    if (!innerOp || innerOp->getOperator() != OO_Star)
        return true;

    auto const tc = loplugin::TypeCheck(innerOp->getArg(0)->getType());

    if (tc.ClassOrStruct("shared_ptr").StdNamespace()
        || tc.ClassOrStruct("__shared_ptr_access").StdNamespace()
        || tc.ClassOrStruct("unique_ptr").StdNamespace()
        || tc.ClassOrStruct("UnoCursorPointer").Namespace("sw").GlobalNamespace())
    {
        report(DiagnosticsEngine::Warning,
               "'*' followed by '&' operating on %0, rather use '.get()'",
               unaryOperator->getBeginLoc())
            << innerOp->getArg(0)->getType() << unaryOperator->getSourceRange();
    }

    return true;
}

loplugin::Plugin::Registration<SmartPointerDerefAddress>
    smartpointerderefaddress("smartpointerderefaddress");

} // namespace

#endif // LO_CLANG_SHARED_PLUGINS

/* vim:set shiftwidth=4 softtabstop=4 expandtab cinoptions=b1,g0,N-s cinkeys+=0=break: */
