//===- AnalysisManager.h - Analysis Management Infrastructure ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_PASS_ANALYSISMANAGER_H
#define MLIR_PASS_ANALYSISMANAGER_H

#include "mlir/IR/Operation.h"
#include "mlir/Pass/PassInstrumentation.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/TypeName.h"

namespace mlir {
//===----------------------------------------------------------------------===//
// Analysis Preservation and Concept Modeling
//===----------------------------------------------------------------------===//

namespace detail {
/// A utility class to represent the analyses that are known to be preserved.
class PreservedAnalyses {
  /// A type used to represent all potential analyses.
  struct AllAnalysesType;

public:
  /// Mark all analyses as preserved.
  void preserveAll() { preservedIDs.insert(TypeID::get<AllAnalysesType>()); }

  /// Returns true if all analyses were marked preserved.
  bool isAll() const {
    return preservedIDs.count(TypeID::get<AllAnalysesType>());
  }

  /// Returns true if no analyses were marked preserved.
  bool isNone() const { return preservedIDs.empty(); }

  /// Preserve the given analyses.
  template <typename AnalysisT> void preserve() {
    preserve(TypeID::get<AnalysisT>());
  }
  template <typename AnalysisT, typename AnalysisT2, typename... OtherAnalysesT>
  void preserve() {
    preserve<AnalysisT>();
    preserve<AnalysisT2, OtherAnalysesT...>();
  }
  void preserve(TypeID id) { preservedIDs.insert(id); }

  /// Returns true if the given analysis has been marked as preserved. Note that
  /// this simply checks for the presence of a given analysis ID and should not
  /// be used as a general preservation checker.
  template <typename AnalysisT> bool isPreserved() const {
    return isPreserved(TypeID::get<AnalysisT>());
  }
  bool isPreserved(TypeID id) const { return preservedIDs.count(id); }

private:
  /// The set of analyses that are known to be preserved.
  SmallPtrSet<TypeID, 2> preservedIDs;
};

namespace analysis_impl {
/// Trait to check if T provides a static 'isInvalidated' method.
template <typename T, typename... Args>
using has_is_invalidated = decltype(std::declval<T &>().isInvalidated(
    std::declval<const PreservedAnalyses &>()));

/// Implementation of 'isInvalidated' if the analysis provides a definition.
template <typename AnalysisT>
std::enable_if_t<llvm::is_detected<has_is_invalidated, AnalysisT>::value, bool>
isInvalidated(AnalysisT &analysis, const PreservedAnalyses &pa) {
  return analysis.isInvalidated(pa);
}
/// Default implementation of 'isInvalidated'.
template <typename AnalysisT>
std::enable_if_t<!llvm::is_detected<has_is_invalidated, AnalysisT>::value, bool>
isInvalidated(AnalysisT &analysis, const PreservedAnalyses &pa) {
  return !pa.isPreserved<AnalysisT>();
}
} // end namespace analysis_impl

/// The abstract polymorphic base class representing an analysis.
struct AnalysisConcept {
  virtual ~AnalysisConcept() = default;

  /// A hook used to query analyses for invalidation. Given a preserved analysis
  /// set, returns true if it should truly be invalidated. This allows for more
  /// fine-tuned invalidation in cases where an analysis wasn't explicitly
  /// marked preserved, but may be preserved(or invalidated) based upon other
  /// properties such as analyses sets.
  virtual bool isInvalidated(const PreservedAnalyses &pa) = 0;
};

/// A derived analysis model used to hold a specific analysis object.
template <typename AnalysisT> struct AnalysisModel : public AnalysisConcept {
  template <typename... Args>
  explicit AnalysisModel(Args &&...args)
      : analysis(std::forward<Args>(args)...) {}

  /// A hook used to query analyses for invalidation.
  bool isInvalidated(const PreservedAnalyses &pa) final {
    return analysis_impl::isInvalidated(analysis, pa);
  }

  /// The actual analysis object.
  AnalysisT analysis;
};

/// This class represents a cache of analyses for a single operation. All
/// computation, caching, and invalidation of analyses takes place here.
class AnalysisMap {
  /// A mapping between an analysis id and an existing analysis instance.
  using ConceptMap = DenseMap<TypeID, std::unique_ptr<AnalysisConcept>>;

  /// Utility to return the name of the given analysis class.
  template <typename AnalysisT> static StringRef getAnalysisName() {
    StringRef name = llvm::getTypeName<AnalysisT>();
    if (!name.consume_front("mlir::"))
      name.consume_front("(anonymous namespace)::");
    return name;
  }

public:
  explicit AnalysisMap(Operation *ir) : ir(ir) {}

  /// Get an analysis for the current IR unit, computing it if necessary.
  template <typename AnalysisT>
  AnalysisT &getAnalysis(PassInstrumentor *pi) {
    return getAnalysisImpl<AnalysisT, Operation *>(pi, ir);
  }

  /// Get an analysis for the current IR unit assuming it's of specific derived
  /// operation type.
  template <typename AnalysisT, typename OpT>
  typename std::enable_if<std::is_constructible<AnalysisT, OpT>::value,
                          AnalysisT &>::type
  getAnalysis(PassInstrumentor *pi) {
    return getAnalysisImpl<AnalysisT, OpT>(pi, cast<OpT>(ir));
  }

  /// Get a cached analysis instance if one exists, otherwise return null.
  template <typename AnalysisT>
  Optional<std::reference_wrapper<AnalysisT>> getCachedAnalysis() const {
    auto res = analyses.find(TypeID::get<AnalysisT>());
    if (res == analyses.end())
      return llvm::None;
    return {static_cast<AnalysisModel<AnalysisT> &>(*res->second).analysis};
  }

  /// Returns the operation that this analysis map represents.
  Operation *getOperation() const { return ir; }

  /// Clear any held analyses.
  void clear() { analyses.clear(); }

  /// Invalidate any cached analyses based upon the given set of preserved
  /// analyses.
  void invalidate(const PreservedAnalyses &pa) {
    // Remove any analyses that were invalidated.
    for (auto it = analyses.begin(), e = analyses.end(); it != e;) {
      auto curIt = it++;
      if (curIt->second->isInvalidated(pa))
        analyses.erase(curIt);
    }
  }

private:
  template <typename AnalysisT, typename OpT>
  AnalysisT &getAnalysisImpl(PassInstrumentor *pi, OpT op) {
    TypeID id = TypeID::get<AnalysisT>();

    typename ConceptMap::iterator it;
    bool wasInserted;
    std::tie(it, wasInserted) = analyses.try_emplace(id);

    // If we don't have a cached analysis for this operation, compute it
    // directly and add it to the cache.
    if (wasInserted) {
      if (pi)
        pi->runBeforeAnalysis(getAnalysisName<AnalysisT>(), id, ir);

      it->second = std::make_unique<AnalysisModel<AnalysisT>>(op);

      if (pi)
        pi->runAfterAnalysis(getAnalysisName<AnalysisT>(), id, ir);
    }
    return static_cast<AnalysisModel<AnalysisT> &>(*it->second).analysis;
  }

  Operation *ir;
  ConceptMap analyses;
};

/// An analysis map that contains a map for the current operation, and a set of
/// maps for any child operations.
struct NestedAnalysisMap {
  NestedAnalysisMap(Operation *op, PassInstrumentor *instrumentor)
      : analyses(op), parentOrInstrumentor(instrumentor) {}
  NestedAnalysisMap(Operation *op, NestedAnalysisMap *parent)
      : analyses(op), parentOrInstrumentor(parent) {}

  /// Get the operation for this analysis map.
  Operation *getOperation() const { return analyses.getOperation(); }

  /// Invalidate any non preserved analyses.
  void invalidate(const PreservedAnalyses &pa);

  /// Returns the parent analysis map for this analysis map, or null if this is
  /// the top-level map.
  const NestedAnalysisMap *getParent() const {
    return parentOrInstrumentor.dyn_cast<NestedAnalysisMap *>();
  }

  /// Returns a pass instrumentation object for the current operation. This
  /// value may be null.
  PassInstrumentor *getPassInstrumentor() const {
    if (auto *parent = getParent())
      return parent->getPassInstrumentor();
    return parentOrInstrumentor.get<PassInstrumentor *>();
  }

  /// The cached analyses for nested operations.
  DenseMap<Operation *, std::unique_ptr<NestedAnalysisMap>> childAnalyses;

  /// The analyses for the owning operation.
  detail::AnalysisMap analyses;

  /// This value has three possible states:
  /// NestedAnalysisMap*: A pointer to the parent analysis map.
  /// PassInstrumentor*: This analysis map is the top-level map, and this
  ///                    pointer is the optional pass instrumentor for the
  ///                    current compilation.
  /// nullptr: This analysis map is the top-level map, and there is nop pass
  ///          instrumentor.
  PointerUnion<NestedAnalysisMap *, PassInstrumentor *> parentOrInstrumentor;
};
} // namespace detail

//===----------------------------------------------------------------------===//
// Analysis Management
//===----------------------------------------------------------------------===//
class ModuleAnalysisManager;

/// This class represents an analysis manager for a particular operation
/// instance. It is used to manage and cache analyses on the operation as well
/// as those for child operations, via nested AnalysisManager instances
/// accessible via 'slice'. This class is intended to be passed around by value,
/// and cannot be constructed directly.
class AnalysisManager {
  using ParentPointerT =
      PointerUnion<const ModuleAnalysisManager *, const AnalysisManager *>;

public:
  using PreservedAnalyses = detail::PreservedAnalyses;

  /// Query for a cached analysis on the given parent operation. The analysis
  /// may not exist and if it does it may be out-of-date.
  template <typename AnalysisT>
  Optional<std::reference_wrapper<AnalysisT>>
  getCachedParentAnalysis(Operation *parentOp) const {
    const detail::NestedAnalysisMap *curParent = impl;
    while (auto *parentAM = curParent->getParent()) {
      if (parentAM->getOperation() == parentOp)
        return parentAM->analyses.getCachedAnalysis<AnalysisT>();
      curParent = parentAM;
    }
    return None;
  }

  /// Query for the given analysis for the current operation.
  template <typename AnalysisT> AnalysisT &getAnalysis() {
    return impl->analyses.getAnalysis<AnalysisT>(getPassInstrumentor());
  }

  /// Query for the given analysis for the current operation of a specific
  /// derived operation type.
  template <typename AnalysisT, typename OpT>
  AnalysisT &getAnalysis() {
    return impl->analyses.getAnalysis<AnalysisT, OpT>(getPassInstrumentor());
  }

  /// Query for a cached entry of the given analysis on the current operation.
  template <typename AnalysisT>
  Optional<std::reference_wrapper<AnalysisT>> getCachedAnalysis() const {
    return impl->analyses.getCachedAnalysis<AnalysisT>();
  }

  /// Query for an analysis of a child operation, constructing it if necessary.
  template <typename AnalysisT> AnalysisT &getChildAnalysis(Operation *op) {
    return nest(op).template getAnalysis<AnalysisT>();
  }

  /// Query for an analysis of a child operation of a specific derived operation
  /// type, constructing it if necessary.
  template <typename AnalysisT, typename OpT>
  AnalysisT &getChildAnalysis(OpT child) {
    return nest(child).template getAnalysis<AnalysisT, OpT>();
  }

  /// Query for a cached analysis of a child operation, or return null.
  template <typename AnalysisT>
  Optional<std::reference_wrapper<AnalysisT>>
  getCachedChildAnalysis(Operation *op) const {
    assert(op->getParentOp() == impl->getOperation());
    auto it = impl->childAnalyses.find(op);
    if (it == impl->childAnalyses.end())
      return llvm::None;
    return it->second->analyses.getCachedAnalysis<AnalysisT>();
  }

  /// Get an analysis manager for the given operation, which must be a proper
  /// descendant of the current operation represented by this analysis manager.
  AnalysisManager nest(Operation *op);

  /// Invalidate any non preserved analyses,
  void invalidate(const PreservedAnalyses &pa) { impl->invalidate(pa); }

  /// Clear any held analyses.
  void clear() {
    impl->analyses.clear();
    impl->childAnalyses.clear();
  }

  /// Returns a pass instrumentation object for the current operation. This
  /// value may be null.
  PassInstrumentor *getPassInstrumentor() const {
    return impl->getPassInstrumentor();
  }

private:
  AnalysisManager(detail::NestedAnalysisMap *impl) : impl(impl) {}

  /// Get an analysis manager for the given immediately nested child operation.
  AnalysisManager nestImmediate(Operation *op);

  /// A reference to the impl analysis map within the parent analysis manager.
  detail::NestedAnalysisMap *impl;

  /// Allow access to the constructor.
  friend class ModuleAnalysisManager;
};

/// An analysis manager class specifically for the top-level operation. This
/// class contains the memory allocations for all nested analysis managers, and
/// provides an anchor point. This is necessary because AnalysisManager is
/// designed to be a thin wrapper around an existing analysis map instance.
class ModuleAnalysisManager {
public:
  ModuleAnalysisManager(Operation *op, PassInstrumentor *passInstrumentor)
      : analyses(op, passInstrumentor) {}
  ModuleAnalysisManager(const ModuleAnalysisManager &) = delete;
  ModuleAnalysisManager &operator=(const ModuleAnalysisManager &) = delete;

  /// Returns an analysis manager for the current top-level module.
  operator AnalysisManager() { return AnalysisManager(&analyses); }

private:
  /// The analyses for the owning module.
  detail::NestedAnalysisMap analyses;
};

} // end namespace mlir

#endif // MLIR_PASS_ANALYSISMANAGER_H
