/**
 * Copyright 2015 Facebook
 * @author Tudor Bosman (tudorb@fb.com)
 */

#ifndef FBLUALIB_USERDATA_H_
#define FBLUALIB_USERDATA_H_

#include <fblualib/LuaUtils.h>

// C++ framework for writing OOP userdata objects.
//
// Userdata objects are full-fledged C++ objects, and Lua methods / metamethods
// are actual methods (non-static member functions) on the object.
//
// Usage:
//
// 1. Declare your object:
//
//   class MyClass {
//    public:
//     int luaLen(lua_State* L);
//     int luaFoo(lua_State* L);
//   };
//
// 2. Register your metamethods and methods, by defining a member of
// ::fblualib::Metatable<MyClass>:
//
//   namespace fblualib {
//
//   template <>
//   const UserDataMethod<MyClass> Metatable<MyClass>::methods = {
//     // Add methods and metamethods here.
//     //
//     // __gc will automatically be added to call the destructor.
//     //
//     // __index will be obeyed, but methods (below) take priority.
//
//     {"__len", &MyClass::luaLen},
//     // etc
//
//     {"foo", &MyClass::luaFoo},
//     // etc
//
//     {nullptr, nullptr}
//   };
//
//   }  // namespace fblualib
//
// 3. Use the object. To push one onto the Lua stack, do:
//
//   // Construct an object of type MyClass from args, push it onto the
//   // Lua stack, and return a reference to it.
//   auto& obj = pushUserData<MyClass>(L, args...);
//
// Note that the objects are allocated on the Lua heap, so, if the data
// is large, you might want your objects to contain a unique_ptr to the
// actual data.
//
// Also in this file, helpers for the case where you only want to use this
// for lifetime management.
//
// - pushObject<T>(L, Args&&... args)
// - T* getObject<T>(L, index)
// - T& getObjectChecked<T>(L, index)

namespace fblualib {

template <class T>
struct UserDataMethod {
  const char* name;
  int (T::*method)(lua_State*);
};

// The methods (and metamethods) are class members. The first argument
// is automatically typechecked and passed as "this". The remaining
// arguments start at Lua stack index 2.

template <class T> struct Metatable {
  static const UserDataMethod<T> methods[];
};

// Push onto the stack the metatable for type T, creating it if not yet
// created.
template <class T>
int pushMetatable(lua_State* L);

// Return the object of type T at the given index, or nullptr if invalid
// (not an object, or object of wrong type)
template <class T>
T* getUserData(lua_State* L, int index);

// Return the object of type T at the given index; throw an error if invalid.
template <class T>
T& getUserDataChecked(lua_State* L, int index);

// Push onto the stack a newly-created object of type T, constructed from args.
template <class T, class... Args>
T& pushUserData(lua_State* L, Args&&... args);

// Register objects of type T to be used using the pushObject / getObject
// mechanism. Calling this multiple times is harmless.
template <class T>
int registerObject(lua_State* L);

// Create an object of type T and push it on the Lua stack.
template <class T, class... Args>
T& pushObject(lua_State* L, Args&&... args);

// Get a pointer to the object of type T stored at the given index,
// or nullptr if invalid.
template <class T>
T* getObject(lua_State* L, int index);

// Get a reference to the object of type T stored at the given index,
// exception if invalid.
template <class T>
T& getObjectChecked(lua_State* L, int index);

namespace detail {

template <class T>
int gcUserData(lua_State* L) {
  auto& obj = getUserDataChecked<T>(L, 1);
  obj.~T();
  return 0;
}

// Upvalue 1 is the index of the method in the method table.
template <class T>
int callUserDataMethod(lua_State* L) {
  auto& obj = getUserDataChecked<T>(L, 1);
  auto index = luaGetChecked<int>(L, lua_upvalueindex(1));
  auto method = Metatable<T>::methods[index].method;
  return (obj.*method)(L);
}

// Upvalues:
// 1 = desired __index function (as per callUserDataMethod)
// 2 = methods table
//
// Called as __index, so arguments:
// 1 = object
// 2 = key
template <class T>
int indexTrampoline(lua_State* L) {
  lua_pushvalue(L, 2);
  lua_gettable(L, lua_upvalueindex(2));
  if (!lua_isnil(L, -1)) {
    return 1;  // found it in methods table, done
  }
  lua_pop(L, 1);
  return callUserDataMethod<T>(L);
}

template <class T>
int registerMethods(lua_State* L) {
  auto table = Metatable<T>::methods;

  int indexMethod = -1;
  for (int i = 0; table->name; ++table, ++i) {
    luaPush(L, table->name);
    luaPush(L, i);
    lua_pushcclosure(L, callUserDataMethod<T>, 1);
    lua_settable(L, -3);
    if (indexMethod == -1 && !strcmp(table->name, "__index")) {
      indexMethod = i;
    }
  }
  return indexMethod;
}

template <class T>
int doCreateMetatable(lua_State* L) {
  lua_newtable(L);
  auto indexMethod = detail::registerMethods<T>(L);
  // metatable

  // Add GC method
  lua_pushcfunction(L, &detail::gcUserData<T>);
  lua_setfield(L, -2, "__gc");

  // If we have an __index metamethod, we need to go through a trampoline
  // that dispatches to either the methods table or the __index metamethod.
  // Otherwise, set __index to itself on the metatable.

  lua_pushvalue(L, -1);
  // metatable metatable
  if (indexMethod >= 0) {
    // Both methods and __index. We need to go through a trampoline.
    // set index as upvalue #1
    // set metatable as upvalue #2
    luaPush(L, indexMethod);
    lua_insert(L, -2);
    lua_pushcclosure(L, detail::indexTrampoline<T>, 2);
    // metatable trampoline
  }

  // metatable <trampoline_or_metatable>
  lua_setfield(L, -2, "__index");

  // metatable

  lua_pushlightuserdata(
      L,
      const_cast<void*>(static_cast<const void*>(&Metatable<T>::methods)));
  lua_pushvalue(L, -2);
  // metatable registry_key metatable
  lua_settable(L, LUA_REGISTRYINDEX);

  // metatable
  return 1;
}

}  // namespace detail

template <class T>
int pushMetatable(lua_State* L) {
  lua_pushlightuserdata(
      L,
      const_cast<void*>(static_cast<const void*>(&Metatable<T>::methods)));
  lua_gettable(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    detail::doCreateMetatable<T>(L);
  }
  return 1;
}

template <class T>
T* getUserData(lua_State* L, int index) {
  auto ptr = static_cast<T*>(lua_touserdata(L, index));
  if (!ptr) {
    return nullptr;  // not userdata
  }
  lua_getmetatable(L, index);
  pushMetatable<T>(L);
  bool ok = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return ok ? ptr : nullptr;
}

template <class T>
T& getUserDataChecked(lua_State* L, int index) {
  auto ptr = getUserData<T>(L, index);
  if (!ptr) {
    luaL_error(L, "Invalid object (not userdata of expected type)");
  }
  return *ptr;
}

template <class T, class... Args>
T& pushUserData(lua_State* L, Args&&... args) {
  auto r = new (lua_newuserdata(L, sizeof(T))) T(std::forward<Args>(args)...);
  pushMetatable<T>(L);
  lua_setmetatable(L, -2);
  return *r;
}

namespace detail {

template <class T>
struct ObjectWrapper {
  template <class... Args>
  explicit ObjectWrapper(Args&&... args) : obj(std::forward<Args>(args)...) { }
  T obj;
};

}  // namespace detail

template <class T> struct Metatable<detail::ObjectWrapper<T>> {
  static const UserDataMethod<detail::ObjectWrapper<T>> methods[];
};

template <class T>
const UserDataMethod<detail::ObjectWrapper<T>>
Metatable<detail::ObjectWrapper<T>>::methods[] = {
  {nullptr, nullptr},
};

template <class T, class... Args>
T& pushObject(lua_State* L, Args&&... args) {
  auto& wrapper = pushUserData<detail::ObjectWrapper<T>>(
      L, std::forward<Args>(args)...);
  return wrapper.obj;
}

template <class T>
T* getObject(lua_State* L, int index) {
  auto wrapper = getUserData<detail::ObjectWrapper<T>>(L, index);
  return wrapper ? &wrapper->obj : nullptr;
}

template <class T>
T& getObjectChecked(lua_State* L, int index) {
  return getUserDataChecked<detail::ObjectWrapper<T>>(L, index).obj;
}

}  // namespaces

#endif /* FBLUALIB_USERDATA_H_ */
