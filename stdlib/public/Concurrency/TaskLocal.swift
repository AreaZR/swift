//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
@_implementationOnly import _SwiftConcurrencyShims

/// Property wrapper that defines a task-local value key.
///
/// A task-local value is a value that can be bound and read in the context of a
/// `Task`. It is implicitly carried with the task, and is accessible by any
/// child tasks the task creates (such as TaskGroup or `async let` created tasks).
///
/// ### Task-local declarations
///
/// Task locals must be declared as static properties (or global properties,
/// once property wrappers support these), like this:
///
///     enum TracingExample {
///         @TaskLocal
///         static let traceID: TraceID?
///     }
///
/// ### Default values
/// Task local values of optional types default to `nil`. It is possible to define
/// not-optional task-local values, and an explicit default value must then be
/// defined instead.
///
/// The default value is returned whenever the task-local is read
/// from a context which either: has no task available to read the value from
/// (e.g. a synchronous function, called without any asynchronous function in its call stack),
///
///
/// ### Reading task-local values
/// Reading task local values is simple and looks the same as-if reading a normal
/// static property:
///
///     guard let traceID = TracingExample.traceID else {
///       print("no trace id")
///       return
///     }
///     print(traceID)
///
/// It is possible to perform task-local value reads from either asynchronous
/// or synchronous functions. Within asynchronous functions, as a "current" task
/// is always guaranteed to exist, this will perform the lookup in the task local context.
///
/// A lookup made from the context of a synchronous function, that is not called
/// from an asynchronous function (!), will immediately return the task-local's
/// default value.
///
/// ### Binding task-local values
/// Task local values cannot be `set` directly and must instead be bound using
/// the scoped `$traceID.withValue() { ... }` operation. The value is only bound
/// for the duration of that scope, and is available to any child tasks which
/// are created within that scope.
///
/// Detached tasks do not inherit task-local values, however tasks created using
/// the `async {}` operation do inherit task-locals by copying them to the new
/// asynchronous task, even though it is an un-structured task.
///
/// ### Examples
///
///     @TaskLocal
///     static var traceID: TraceID?
///
///     print("traceID: \(traceID)") // traceID: nil
///
///     $traceID.withValue(1234) { // bind the value
///       print("traceID: \(traceID)") // traceID: 1234
///       call() // traceID: 1234
///
///       asyncDetached { // detached tasks do not inherit task-local values
///         call() // traceID: nil
///       }
///
///       async { // async tasks do inherit task locals by copying
///         call() // traceID: 1234
///       }
///     }
///
///
///     func call() {
///       print("traceID: \(traceID)") // 1234
///     }
///
/// This type must be a `class` so it has a stable identity, that is used as key
/// value for lookups in the task local storage.
@propertyWrapper
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public final class TaskLocal<Value: Sendable>: Sendable, CustomStringConvertible {
  let defaultValue: Value

  public init(wrappedValue defaultValue: Value) {
    self.defaultValue = defaultValue
  }

  var key: Builtin.RawPointer {
    unsafeBitCast(self, to: Builtin.RawPointer.self)
  }

  /// Gets the value currently bound to this task-local from the current task.
  ///
  /// If no current task is available in the context where this call is made,
  /// or if the task-local has no value bound, this will return the `defaultValue`
  /// of the task local.
  public func get() -> Value {
    withUnsafeCurrentTask { task in
      guard let task = task else {
        return self.defaultValue
      }

      let value = _taskLocalValueGet(task._task, key: key)

      guard let rawValue = value else {
        return self.defaultValue
      }

      // Take the value; The type should be correct by construction
      let storagePtr =
          rawValue.bindMemory(to: Value.self, capacity: 1)
      return UnsafeMutablePointer<Value>(mutating: storagePtr).pointee
    }
  }

  /// Binds the task-local to the specific value for the duration of the body.
  ///
  /// The value is available throughout the execution of the body closure,
  /// including any `get` operations performed by child-tasks created during the
  /// execution of the body closure.
  ///
  /// If the same task-local is bound multiple times, be it in the same task, or
  /// in specific child tasks, the more specific (i.e. "deeper") binding is
  /// returned when the value is read.
  ///
  /// If the value is a reference type, it will be retained for the duration of
  /// the body closure.
  @discardableResult
  public func withValue<R>(_ valueDuringBody: Value, do body: () async throws -> R,
                           file: String = #file, line: UInt = #line) async rethrows -> R {
    // check if we're not trying to bind a value from an illegal context; this may crash
    _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: file, line: line)

    // we need to escape the `_task` since the withUnsafeCurrentTask closure is not `async`.
    // this is safe, since we know the task will remain alive because we are running inside of it.
    let _task = withUnsafeCurrentTask { task in
      task!._task // !-safe, guaranteed to have task available inside async function
    }

    _taskLocalValuePush(_task, key: key, value: valueDuringBody)
    defer { _taskLocalValuePop(_task) }

    return try await body()
  }

  public var projectedValue: TaskLocal<Value> {
    get {
      self
    }

    @available(*, unavailable, message: "use '$myTaskLocal.withValue(_:do:)' instead")
    set {
      fatalError("Illegal attempt to set a \(Self.self) value, use `withValue(...) { ... }` instead.")
    }
  }

  // This subscript is used to enforce that the property wrapper may only be used
  // on static (or rather, "without enclosing instance") properties.
  // This is done by marking the `_enclosingInstance` as `Never` which informs
  // the type-checker that this property-wrapper never wants to have an enclosing
  // instance (it is impossible to declare a property wrapper inside the `Never`
  // type).
  public static subscript(
    _enclosingInstance object: Never,
    wrapped wrappedKeyPath: ReferenceWritableKeyPath<Never, Value>,
    storage storageKeyPath: ReferenceWritableKeyPath<Never, TaskLocal<Value>>
  ) -> Value {
    get {
      fatalError()
    }
  }

  public var wrappedValue: Value {
    self.get()
  }

  public var description: String {
    "\(Self.self)(defaultValue: \(self.defaultValue))"
  }

}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension UnsafeCurrentTask {

  /// Allows for executing a synchronous `body` while binding a task-local value
  /// in the current task.
  ///
  /// This function MUST NOT be invoked by any other task than the current task
  /// represented by this object.
  @discardableResult
  public func withTaskLocal<Value: Sendable, R>(
      _ taskLocal: TaskLocal<Value>,
      boundTo valueDuringBody: Value,
      do body: () throws -> R,
      file: String = #file, line: UInt = #line) rethrows -> R {
    // check if we're not trying to bind a value from an illegal context; this may crash
    _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: file, line: line)

    _taskLocalValuePush(self._task, key: taskLocal.key, value: valueDuringBody)
    defer { _taskLocalValuePop(_task) }

    return try body()
  }
}

// ==== ------------------------------------------------------------------------

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@_silgen_name("swift_task_localValuePush")
public func _taskLocalValuePush<Value>(
  _ task: Builtin.NativeObject,
  key: Builtin.RawPointer/*: Key*/,
  value: __owned Value
) // where Key: TaskLocal

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@_silgen_name("swift_task_localValuePop")
public func _taskLocalValuePop(
  _ task: Builtin.NativeObject
)

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@_silgen_name("swift_task_localValueGet")
public func _taskLocalValueGet(
  _ task: Builtin.NativeObject,
  key: Builtin.RawPointer/*Key*/
) -> UnsafeMutableRawPointer? // where Key: TaskLocal

// ==== Checks -----------------------------------------------------------------

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@usableFromInline
func _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: String, line: UInt) {
  if _taskHasTaskGroupStatusRecord() {
    file.withCString { _fileStart in
      _reportIllegalTaskLocalBindingWithinWithTaskGroup(
          _fileStart, file.count, true, line)
    }
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@usableFromInline
@_silgen_name("swift_task_reportIllegalTaskLocalBindingWithinWithTaskGroup")
func _reportIllegalTaskLocalBindingWithinWithTaskGroup(
  _ _filenameStart: UnsafePointer<Int8>,
  _ _filenameLength: Int,
  _ _filenameIsASCII: Bool,
  _ _line: UInt)
