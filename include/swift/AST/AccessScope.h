//===--- AccessScope.h - Swift Access Scope ---------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ACCESSSCOPE_H
#define SWIFT_ACCESSSCOPE_H

#include "swift/AST/AttrKind.h"
#include "swift/AST/DeclContext.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/PointerIntPair.h"

namespace swift {

/// Used to provide the kind of scope limitation in AccessScope::Value
enum class AccessLimitKind : uint8_t { None = 0, Private, Package };

/// The wrapper around the outermost DeclContext from which
/// a particular declaration can be accessed.
class AccessScope {
  /// The declaration context along with an enum indicating the level of
  /// scope limitation.
  /// If the declaration context is set, and the limit kind is Private, the
  /// access level is considered 'private'. Whether it's 'internal' or
  /// 'fileprivate' is determined by what the declaration context casts to. If
  /// the declaration context is null, and the limit kind is None, the access
  /// level is considered 'public'. If the limit kind is Private, the access
  /// level is considered SPI. If it's Package, the access level is considered
  /// 'package'. Below is a table showing the combinations.
  ///
  /// AccessLimitKind       DC == nullptr               DC != nullptr
  /// ---------------------------------------------------------------------------
  ///  None                    public                  fileprivate or internal (check DC to tell which)
  ///  Private               `@_spi` public                private
  ///  Package                 package                    (unused)

  llvm::PointerIntPair<const DeclContext *, 2, AccessLimitKind> Value;

public:
  AccessScope(const DeclContext *DC,
              AccessLimitKind limitKind = AccessLimitKind::None);

  static AccessScope getPublic() {
    return AccessScope(nullptr, AccessLimitKind::None);
  }
  static AccessScope getPackage() {
    return AccessScope(nullptr, AccessLimitKind::Package);
  }

  /// Check if private access is allowed. This is a lexical scope check in Swift
  /// 3 mode. In Swift 4 mode, declarations and extensions of the same type will
  /// also allow access.
  static bool allowsPrivateAccess(const DeclContext *useDC, const DeclContext *sourceDC);

  /// Returns nullptr if access scope is public.
  const DeclContext *getDeclContext() const { return Value.getPointer(); }

  bool operator==(AccessScope RHS) const { return Value == RHS.Value; }
  bool operator!=(AccessScope RHS) const { return !(*this == RHS); }
  bool hasEqualDeclContextWith(AccessScope RHS) const {
    if (isPublic())
      return RHS.isPublic();
    if (isPackage())
      return RHS.isPackage();
    return getDeclContext() == RHS.getDeclContext();
  }

  bool isPublic() const {
    return !Value.getPointer() && Value.getInt() == AccessLimitKind::None;
  }
  bool isPrivate() const {
    return Value.getPointer() && Value.getInt() == AccessLimitKind::Private;
  }
  bool isFileScope() const;
  bool isInternal() const;
  bool isPackage() const {
    return !Value.getPointer() && Value.getInt() == AccessLimitKind::Package;
  }

  /// Returns true if the context of this (use site) is more restrictive than
  /// the argument context (decl site). This function does _not_ check the
  /// restrictiveness of the access level between this and the argument. \see
  /// AccessScope::isInContext
  bool isChildOf(AccessScope AS) const {
    if (isInContext()) {
      if (AS.isInContext())
        return allowsPrivateAccess(getDeclContext(), AS.getDeclContext());
      else
        return AS.isPackage() || AS.isPublic();
    }
    if (isPackage())
      return AS.isPublic();
    // If this is public, it can't be less than access level of AS
    // so return false
    return false;
  }

  /// Result depends on whether it's called at a use site or a decl site:
  ///
  /// For example,
  ///
  /// ```
  /// public func foo(_ arg: bar) {} // `bar` is a `package` decl in another
  /// module
  /// ```
  ///
  /// The meaning of \c isInContext changes whether it's at the use site or the
  /// decl site.
  ///
  /// The use site of \c bar, i.e. \c foo, is "in context" (decl context is
  /// non-null), regardless of the access level of \c foo (\c public in this
  /// case).
  ///
  /// The decl site of \c bar is only "in context" if the access level of the
  /// decl is \c internal or more restrictive. The context at the decl site is\c
  /// FileUnit if the decl is \c fileprivate or \c private; \c ModuleDecl if \c
  /// internal, and null if \c package or \c public.
  bool isInContext() const { return getDeclContext() != nullptr; }

  /// Returns the associated access level for diagnostic purposes.
  AccessLevel accessLevelForDiagnostics() const;

  /// Returns the minimum access level required to access
  /// associated DeclContext for diagnostic purposes.
  AccessLevel requiredAccessForDiagnostics() const {
    return isFileScope()
      ? AccessLevel::FilePrivate
      : accessLevelForDiagnostics();
  }

  /// Returns the narrowest access scope if this and the specified access scope
  /// have common intersection, or None if scopes don't intersect.
  const Optional<AccessScope> intersectWith(AccessScope accessScope) const {
    if (hasEqualDeclContextWith(accessScope)) {
      if (isPrivate())
        return *this;
      return accessScope;
    }
    if (isChildOf(accessScope))
      return *this;
    if (accessScope.isChildOf(*this))
      return accessScope;

    return None;
  }

  SWIFT_DEBUG_DUMP;
};

} // end namespace swift

#endif
