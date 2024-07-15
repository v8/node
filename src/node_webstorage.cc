#include "node_webstorage.h"
#include "base_object-inl.h"
#include "debug_utils-inl.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "node.h"
#include "node_errors.h"
#include "node_mem-inl.h"
#include "sqlite3.h"
#include "util-inl.h"

namespace node {
namespace webstorage {

using v8::Array;
using v8::Boolean;
using v8::Context;
using v8::DontDelete;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::IndexedPropertyHandlerConfiguration;
using v8::Integer;
using v8::Intercepted;
using v8::Isolate;
using v8::Local;
using v8::Map;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Name;
using v8::NamedPropertyHandlerConfiguration;
using v8::Null;
using v8::Object;
using v8::PropertyAttribute;
using v8::PropertyCallbackInfo;
using v8::PropertyDescriptor;
using v8::PropertyHandlerFlags;
using v8::String;
using v8::Uint32;
using v8::Value;

#define THROW_SQLITE_ERROR(env, r)                                             \
  node::THROW_ERR_INVALID_STATE((env), sqlite3_errstr((r)))

#define CHECK_ERROR_OR_THROW(env, expr, expected, ret)                         \
  do {                                                                         \
    int r_ = (expr);                                                           \
    if (r_ != (expected)) {                                                    \
      THROW_SQLITE_ERROR((env), r_);                                           \
      return (ret);                                                            \
    }                                                                          \
  } while (0)

static void ThrowQuotaExceededException(Local<Context> context) {
  Isolate* isolate = context->GetIsolate();
  auto dom_exception_str = FIXED_ONE_BYTE_STRING(isolate, "DOMException");
  auto err_name = FIXED_ONE_BYTE_STRING(isolate, "QuotaExceededError");
  auto err_message =
      FIXED_ONE_BYTE_STRING(isolate, "Setting the value exceeded the quota");
  Local<Object> per_context_bindings;
  Local<Value> domexception_ctor_val;
  if (!GetPerContextExports(context).ToLocal(&per_context_bindings) ||
      !per_context_bindings->Get(context, dom_exception_str)
           .ToLocal(&domexception_ctor_val)) {
    return;
  }
  CHECK(domexception_ctor_val->IsFunction());
  Local<Function> domexception_ctor = domexception_ctor_val.As<Function>();
  Local<Value> argv[] = {err_message, err_name};
  Local<Value> exception;

  if (!domexception_ctor->NewInstance(context, arraysize(argv), argv)
           .ToLocal(&exception)) {
    return;
  }

  isolate->ThrowException(exception);
}

Storage::Storage(Environment* env, Local<Object> object, Local<String> location)
    : BaseObject(env, object) {
  MakeWeak();
  node::Utf8Value utf8_location(env->isolate(), location);
  symbols_.Reset(env->isolate(), Map::New(env->isolate()));
  db_ = nullptr;
  location_ = utf8_location.ToString();
}

Storage::~Storage() {
  db_ = nullptr;
}

void Storage::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("symbols", symbols_);
  tracker->TrackField("location", location_);
}

bool Storage::Open() {
  static const int kCurrentSchemaVersion = 1;
  static const char get_schema_version_sql[] =
      "SELECT schema_version FROM nodejs_webstorage_state";
  static const char init_sql_v0[] =
      "PRAGMA encoding = 'UTF-16le';"
      "PRAGMA busy_timeout = 3000;"
      "PRAGMA journal_mode = WAL;"
      "PRAGMA synchronous = NORMAL;"
      "PRAGMA temp_store = memory;"
      "PRAGMA optimize;"
      ""
      "CREATE TABLE IF NOT EXISTS nodejs_webstorage("
      "  key BLOB NOT NULL,"
      "  value BLOB NOT NULL,"
      "  PRIMARY KEY(key)"
      ") STRICT;"
      ""
      "CREATE TABLE IF NOT EXISTS nodejs_webstorage_state("
      // max_size is 10MB. This can be made configurable in the future.
      "  max_size INTEGER NOT NULL DEFAULT 10485760,"
      "  total_size INTEGER NOT NULL,"
      "  schema_version INTEGER NOT NULL DEFAULT 0,"
      "  single_row_ INTEGER NOT NULL DEFAULT 1 CHECK(single_row_ = 1),"
      "  PRIMARY KEY(single_row_)"
      ") STRICT;"
      ""
      "CREATE TRIGGER IF NOT EXISTS nodejs_quota_insert "
      "AFTER INSERT ON nodejs_webstorage "
      "FOR EACH ROW "
      "BEGIN "
      "  UPDATE nodejs_webstorage_state"
      "    SET total_size = total_size + OCTET_LENGTH(NEW.key) +"
      "      OCTET_LENGTH(NEW.value);"
      "  SELECT RAISE(ABORT, 'QuotaExceeded') WHERE EXISTS ("
      "    SELECT 1 FROM nodejs_webstorage_state WHERE total_size > max_size"
      "  );"
      "END;"
      ""
      "CREATE TRIGGER IF NOT EXISTS nodejs_quota_update "
      "AFTER UPDATE ON nodejs_webstorage "
      "FOR EACH ROW "
      "BEGIN "
      "  UPDATE nodejs_webstorage_state"
      "    SET total_size = total_size + "
      "      ((OCTET_LENGTH(NEW.key) + OCTET_LENGTH(NEW.value)) -"
      "      (OCTET_LENGTH(OLD.key) + OCTET_LENGTH(OLD.value)));"
      "  SELECT RAISE(ABORT, 'QuotaExceeded') WHERE EXISTS ("
      "    SELECT 1 FROM nodejs_webstorage_state WHERE total_size > max_size"
      "  );"
      "END;"
      ""
      "CREATE TRIGGER IF NOT EXISTS nodejs_quota_delete "
      "AFTER DELETE ON nodejs_webstorage "
      "FOR EACH ROW "
      "BEGIN "
      "  UPDATE nodejs_webstorage_state"
      "    SET total_size = total_size - (OCTET_LENGTH(OLD.key) +"
      "      OCTET_LENGTH(OLD.value));"
      "END;"
      ""
      "INSERT OR IGNORE INTO nodejs_webstorage_state (total_size) VALUES (0);";

  sqlite3* db = db_.get();
  if (db != nullptr) {
    return true;
  }

  int r = sqlite3_open(location_.c_str(), &db);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  r = sqlite3_exec(db, init_sql_v0, 0, 0, nullptr);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);

  // Get the current schema version, used to determine schema migrations.
  sqlite3_stmt* s = nullptr;
  r = sqlite3_prepare_v2(db, get_schema_version_sql, -1, &s, 0);
  r = sqlite3_exec(db, init_sql_v0, 0, 0, nullptr);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  auto stmt = stmt_unique_ptr(s);
  CHECK_ERROR_OR_THROW(env(), sqlite3_step(stmt.get()), SQLITE_ROW, false);
  CHECK(sqlite3_column_type(stmt.get(), 0) == SQLITE_INTEGER);
  int schema_version = sqlite3_column_int(stmt.get(), 0);
  stmt = nullptr;  // Force finalization.

  if (schema_version > kCurrentSchemaVersion) {
    node::THROW_ERR_INVALID_STATE(
        env(), "localStorage was created with a newer version of Node.js");
    return false;
  }

  if (schema_version < kCurrentSchemaVersion) {
    // Run any migrations and update the schema version.
    std::string set_user_version_sql =
        "UPDATE nodejs_webstorage_state SET schema_version = " +
        std::to_string(kCurrentSchemaVersion) + ";";
    r = sqlite3_exec(db, set_user_version_sql.c_str(), 0, 0, nullptr);
    CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  }

  db_ = conn_unique_ptr(db);
  return true;
}

void Storage::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Realm* realm = Realm::GetCurrent(args);

  if (!args[0]->StrictEquals(realm->isolate_data()->constructor_key_symbol())) {
    THROW_ERR_ILLEGAL_CONSTRUCTOR(env);
    return;
  }

  CHECK(args.IsConstructCall());
  CHECK(args[1]->IsString());
  new Storage(env, args.This(), args[1].As<String>());
}

void Storage::Clear() {
  if (!Open()) {
    return;
  }

  static const char sql[] = "DELETE FROM nodejs_webstorage";
  sqlite3_stmt* s = nullptr;
  CHECK_ERROR_OR_THROW(
      env(), sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0), SQLITE_OK, void());
  auto stmt = stmt_unique_ptr(s);
  CHECK_ERROR_OR_THROW(env(), sqlite3_step(stmt.get()), SQLITE_DONE, void());
}

Local<Array> Storage::Enumerate() {
  if (!Open()) {
    return Local<Array>();
  }

  static const char sql[] = "SELECT key FROM nodejs_webstorage";
  sqlite3_stmt* s = nullptr;
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Array>());
  auto stmt = stmt_unique_ptr(s);
  std::vector<Local<Value>> values;
  while ((r = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    CHECK(sqlite3_column_type(stmt.get(), 0) == SQLITE_BLOB);
    auto size = sqlite3_column_bytes(stmt.get(), 0) / sizeof(uint16_t);
    values.emplace_back(
        String::NewFromTwoByte(env()->isolate(),
                               reinterpret_cast<const uint16_t*>(
                                   sqlite3_column_blob(stmt.get(), 0)),
                               v8::NewStringType::kNormal,
                               size)
            .ToLocalChecked());
  }
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_DONE, Local<Array>());
  return Array::New(env()->isolate(), values.data(), values.size());
}

Local<Value> Storage::Length() {
  if (!Open()) {
    return Local<Value>();
  }

  static const char sql[] = "SELECT count(*) FROM nodejs_webstorage";
  sqlite3_stmt* s = nullptr;
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Value>());
  auto stmt = stmt_unique_ptr(s);
  CHECK_ERROR_OR_THROW(
      env(), sqlite3_step(stmt.get()), SQLITE_ROW, Local<Value>());
  CHECK(sqlite3_column_type(stmt.get(), 0) == SQLITE_INTEGER);
  int result = sqlite3_column_int(stmt.get(), 0);
  return Integer::New(env()->isolate(), result);
}

Local<Value> Storage::Load(Local<Name> key) {
  if (key->IsSymbol()) {
    auto symbol_map = symbols_.Get(env()->isolate());
    MaybeLocal<Value> result = symbol_map->Get(env()->context(), key);
    return result.FromMaybe(Local<Value>());
  }

  if (!Open()) {
    return Local<Value>();
  }

  static const char sql[] =
      "SELECT value FROM nodejs_webstorage WHERE key = ? LIMIT 1";
  sqlite3_stmt* s = nullptr;
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Value>());
  auto stmt = stmt_unique_ptr(s);
  node::TwoByteValue utf16key(env()->isolate(), key);
  auto key_size = utf16key.length() * sizeof(uint16_t);
  r = sqlite3_bind_blob(stmt.get(), 1, utf16key.out(), key_size, SQLITE_STATIC);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Value>());
  auto value = Local<Value>();
  r = sqlite3_step(stmt.get());
  if (r == SQLITE_ROW) {
    CHECK(sqlite3_column_type(stmt.get(), 0) == SQLITE_BLOB);
    auto size = sqlite3_column_bytes(stmt.get(), 0) / sizeof(uint16_t);
    value = String::NewFromTwoByte(env()->isolate(),
                                   reinterpret_cast<const uint16_t*>(
                                       sqlite3_column_blob(stmt.get(), 0)),
                                   v8::NewStringType::kNormal,
                                   size)
                .ToLocalChecked();
  } else if (r != SQLITE_DONE) {
    THROW_SQLITE_ERROR(env(), r);
  }

  return value;
}

Local<Value> Storage::LoadKey(const int index) {
  if (!Open()) {
    return Local<Value>();
  }

  static const char sql[] =
      "SELECT key FROM nodejs_webstorage LIMIT 1 OFFSET ?";
  sqlite3_stmt* s = nullptr;
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Value>());
  auto stmt = stmt_unique_ptr(s);
  r = sqlite3_bind_int(stmt.get(), 1, index);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, Local<Value>());

  auto value = Local<Value>();
  r = sqlite3_step(stmt.get());
  if (r == SQLITE_ROW) {
    CHECK(sqlite3_column_type(stmt.get(), 0) == SQLITE_BLOB);
    auto size = sqlite3_column_bytes(stmt.get(), 0) / sizeof(uint16_t);
    value = String::NewFromTwoByte(env()->isolate(),
                                   reinterpret_cast<const uint16_t*>(
                                       sqlite3_column_blob(stmt.get(), 0)),
                                   v8::NewStringType::kNormal,
                                   size)
                .ToLocalChecked();
  } else if (r != SQLITE_DONE) {
    THROW_SQLITE_ERROR(env(), r);
  }

  return value;
}

bool Storage::Remove(Local<Name> key) {
  if (key->IsSymbol()) {
    auto symbol_map = symbols_.Get(env()->isolate());
    Maybe<bool> result = symbol_map->Delete(env()->context(), key);
    return !result.IsNothing();
  }

  if (!Open()) {
    return false;
  }

  static const char sql[] = "DELETE FROM nodejs_webstorage WHERE key = ?";
  sqlite3_stmt* s = nullptr;
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  auto stmt = stmt_unique_ptr(s);
  node::TwoByteValue utf16key(env()->isolate(), key);
  auto key_size = utf16key.length() * sizeof(uint16_t);
  r = sqlite3_bind_blob(stmt.get(), 1, utf16key.out(), key_size, SQLITE_STATIC);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  CHECK_ERROR_OR_THROW(env(), sqlite3_step(stmt.get()), SQLITE_DONE, false);
  return true;
}

bool Storage::Store(Local<Name> key, Local<Value> value) {
  if (key->IsSymbol()) {
    auto symbol_map = symbols_.Get(env()->isolate());
    MaybeLocal<Map> result = symbol_map->Set(env()->context(), key, value);
    return !result.IsEmpty();
  }

  Local<String> val;
  if (!value->ToString(env()->context()).ToLocal(&val)) {
    return false;
  }

  if (!Open()) {
    return false;
  }

  static const char sql[] =
      "INSERT INTO nodejs_webstorage (key, value) VALUES (?, ?)"
      "  ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value"
      "  WHERE EXCLUDED.key = key";
  sqlite3_stmt* s = nullptr;
  node::TwoByteValue utf16key(env()->isolate(), key);
  node::TwoByteValue utf16val(env()->isolate(), val);
  int r = sqlite3_prepare_v2(db_.get(), sql, -1, &s, 0);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  auto stmt = stmt_unique_ptr(s);
  auto key_size = utf16key.length() * sizeof(uint16_t);
  r = sqlite3_bind_blob(stmt.get(), 1, utf16key.out(), key_size, SQLITE_STATIC);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);
  auto val_size = utf16val.length() * sizeof(uint16_t);
  r = sqlite3_bind_blob(stmt.get(), 2, utf16val.out(), val_size, SQLITE_STATIC);
  CHECK_ERROR_OR_THROW(env(), r, SQLITE_OK, false);

  r = sqlite3_step(stmt.get());
  if (r == SQLITE_CONSTRAINT) {
    ThrowQuotaExceededException(env()->context());
    return false;
  }

  CHECK_ERROR_OR_THROW(env(), r, SQLITE_DONE, false);
  return true;
}

static Local<Name> Uint32ToName(Local<Context> context, uint32_t index) {
  return Uint32::New(context->GetIsolate(), index)
      ->ToString(context)
      .ToLocalChecked();
}

static void Clear(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  storage->Clear();
}

static void GetItem(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  Environment* env = Environment::GetCurrent(info);

  if (info.Length() < 1) {
    return THROW_ERR_MISSING_ARGS(
        env, "Failed to execute 'getItem' on 'Storage': 1 argument required");
  }

  Local<String> prop;
  if (!info[0]->ToString(env->context()).ToLocal(&prop)) {
    return;
  }

  Local<Value> result = storage->Load(prop);
  if (result.IsEmpty()) {
    info.GetReturnValue().Set(Null(env->isolate()));
  } else {
    info.GetReturnValue().Set(result);
  }
}

static void Key(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  Environment* env = Environment::GetCurrent(info);
  int index;

  if (info.Length() < 1) {
    return THROW_ERR_MISSING_ARGS(
        env, "Failed to execute 'key' on 'Storage': 1 argument required");
  }

  if (!info[0]->Int32Value(env->context()).To(&index)) {
    return;
  }

  if (index < 0) {
    info.GetReturnValue().Set(Null(env->isolate()));
    return;
  }

  Local<Value> result = storage->LoadKey(index);
  if (result.IsEmpty()) {
    info.GetReturnValue().Set(Null(env->isolate()));
  } else {
    info.GetReturnValue().Set(result);
  }
}

static void RemoveItem(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  Environment* env = Environment::GetCurrent(info);
  Local<String> prop;

  if (info.Length() < 1) {
    return THROW_ERR_MISSING_ARGS(
        env,
        "Failed to execute 'removeItem' on 'Storage': 1 argument required");
  }

  if (!info[0]->ToString(env->context()).ToLocal(&prop)) {
    return;
  }

  storage->Remove(prop);
}

static void SetItem(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  Environment* env = Environment::GetCurrent(info);

  if (info.Length() < 2) {
    return THROW_ERR_MISSING_ARGS(
        env, "Failed to execute 'setItem' on 'Storage': 2 arguments required");
  }

  Local<String> prop;
  if (!info[0]->ToString(env->context()).ToLocal(&prop)) {
    return;
  }

  storage->Store(prop, info[1]);
}

template <typename T>
static bool ShouldIntercept(Local<Name> property,
                            const PropertyCallbackInfo<T>& info) {
  Environment* env = Environment::GetCurrent(info);
  Local<Value> proto = info.This()->GetPrototype();

  if (proto->IsObject()) {
    bool has_prop;

    if (!proto.As<Object>()->Has(env->context(), property).To(&has_prop)) {
      return false;
    }

    if (has_prop) {
      return false;
    }
  }

  return true;
}

static Intercepted StorageGetter(Local<Name> property,
                                 const PropertyCallbackInfo<Value>& info) {
  if (!ShouldIntercept(property, info)) {
    return Intercepted::kNo;
  }

  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This(), Intercepted::kNo);
  Local<Value> result = storage->Load(property);

  if (result.IsEmpty()) {
    info.GetReturnValue().SetUndefined();
  } else {
    info.GetReturnValue().Set(result);
  }

  return Intercepted::kYes;
}

static Intercepted StorageSetter(Local<Name> property,
                                 Local<Value> value,
                                 const PropertyCallbackInfo<void>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This(), Intercepted::kNo);

  if (!storage->Store(property, value)) {
    info.GetReturnValue().Set(false);
  }

  return Intercepted::kYes;
}

static Intercepted StorageQuery(Local<Name> property,
                                const PropertyCallbackInfo<Integer>& info) {
  if (!ShouldIntercept(property, info)) {
    return Intercepted::kNo;
  }

  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This(), Intercepted::kNo);
  Local<Value> result = storage->Load(property);
  if (result.IsEmpty()) {
    return Intercepted::kNo;
  }

  info.GetReturnValue().Set(0);
  return Intercepted::kYes;
}

static Intercepted StorageDeleter(Local<Name> property,
                                  const PropertyCallbackInfo<Boolean>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This(), Intercepted::kNo);

  if (storage->Remove(property)) {
    info.GetReturnValue().Set(true);
  }

  return Intercepted::kYes;
}

static void StorageEnumerator(const PropertyCallbackInfo<Array>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  info.GetReturnValue().Set(storage->Enumerate());
}

static Intercepted StorageDefiner(Local<Name> property,
                                  const PropertyDescriptor& desc,
                                  const PropertyCallbackInfo<void>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This(), Intercepted::kNo);

  if (desc.has_value()) {
    return StorageSetter(property, desc.value(), info);
  }

  return Intercepted::kYes;
}

static Intercepted IndexedGetter(uint32_t index,
                                 const PropertyCallbackInfo<Value>& info) {
  Environment* env = Environment::GetCurrent(info);
  return StorageGetter(Uint32ToName(env->context(), index), info);
}

static Intercepted IndexedSetter(uint32_t index,
                                 Local<Value> value,
                                 const PropertyCallbackInfo<void>& info) {
  Environment* env = Environment::GetCurrent(info);
  return StorageSetter(Uint32ToName(env->context(), index), value, info);
}

static Intercepted IndexedQuery(uint32_t index,
                                const PropertyCallbackInfo<Integer>& info) {
  Environment* env = Environment::GetCurrent(info);
  return StorageQuery(Uint32ToName(env->context(), index), info);
}

static Intercepted IndexedDeleter(uint32_t index,
                                  const PropertyCallbackInfo<Boolean>& info) {
  Environment* env = Environment::GetCurrent(info);
  return StorageDeleter(Uint32ToName(env->context(), index), info);
}

static Intercepted IndexedDefiner(uint32_t index,
                                  const PropertyDescriptor& desc,
                                  const PropertyCallbackInfo<void>& info) {
  Environment* env = Environment::GetCurrent(info);
  return StorageDefiner(Uint32ToName(env->context(), index), desc, info);
}

static void StorageLengthGetter(const FunctionCallbackInfo<Value>& info) {
  Storage* storage;
  ASSIGN_OR_RETURN_UNWRAP(&storage, info.This());
  info.GetReturnValue().Set(storage->Length());
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  auto ctor_tmpl = NewFunctionTemplate(isolate, Storage::New);
  auto inst_tmpl = ctor_tmpl->InstanceTemplate();

  inst_tmpl->SetInternalFieldCount(Storage::kInternalFieldCount);
  inst_tmpl->SetHandler(NamedPropertyHandlerConfiguration(
      StorageGetter,
      StorageSetter,
      StorageQuery,
      StorageDeleter,
      StorageEnumerator,
      StorageDefiner,
      nullptr,
      Local<Value>(),
      PropertyHandlerFlags::kHasNoSideEffect));
  inst_tmpl->SetHandler(IndexedPropertyHandlerConfiguration(
      IndexedGetter,
      IndexedSetter,
      IndexedQuery,
      IndexedDeleter,
      nullptr,
      IndexedDefiner,
      nullptr,
      Local<Value>(),
      PropertyHandlerFlags::kHasNoSideEffect));

  Local<FunctionTemplate> length_getter =
      FunctionTemplate::New(isolate, StorageLengthGetter);
  ctor_tmpl->PrototypeTemplate()->SetAccessorProperty(
      FIXED_ONE_BYTE_STRING(isolate, "length"),
      length_getter,
      Local<FunctionTemplate>(),
      DontDelete);

  SetProtoMethod(isolate, ctor_tmpl, "clear", Clear);
  SetProtoMethodNoSideEffect(isolate, ctor_tmpl, "getItem", GetItem);
  SetProtoMethodNoSideEffect(isolate, ctor_tmpl, "key", Key);
  SetProtoMethod(isolate, ctor_tmpl, "removeItem", RemoveItem);
  SetProtoMethod(isolate, ctor_tmpl, "setItem", SetItem);
  SetConstructorFunction(context, target, "Storage", ctor_tmpl);

  auto symbol = env->isolate_data()->constructor_key_symbol();
  target
      ->DefineOwnProperty(context,
                          FIXED_ONE_BYTE_STRING(isolate, "kConstructorKey"),
                          symbol,
                          PropertyAttribute::ReadOnly)
      .Check();
}

}  // namespace webstorage
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(webstorage, node::webstorage::Initialize)
