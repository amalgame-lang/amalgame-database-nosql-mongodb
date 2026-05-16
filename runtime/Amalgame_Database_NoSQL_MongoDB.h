/*
 * Amalgame Standard Library — Amalgame.Database.NoSQL.MongoDB
 * Copyright (c) 2026 Bastien MOUGET
 * https://github.com/amalgame-lang/Amalgame
 *
 * MongoDB binding — dynamic-linked to the official mongo-c-driver
 * (libmongoc-1.0 + libbson-1.0). First amalgame-lang package to:
 *   - link against TWO system libraries
 *   - use a document model (JSON in/out, BSON under the hood)
 *
 * Surface (v1):
 *   Connect(uri) / Close / IsConnected / LastError     lifecycle + diag
 *   Ping                                                round-trip health check
 *   ServerVersion                                       mongod version string
 *
 *   InsertOne(dbName, collName, jsonDoc)                document insert
 *   FindOne(dbName, collName, queryJson)               → JSON string ("" if none)
 *   FindMany(dbName, collName, queryJson)              → List<string> of JSON
 *   DeleteOne(dbName, collName, queryJson)              document delete
 *   UpdateOne(dbName, collName, queryJson, updateJson)  document update
 *   Count(dbName, collName, queryJson)                 → matching doc count
 *
 * Every document on the wire flows as a JSON string. The wrapper
 * converts to/from BSON via libbson's bson_new_from_json /
 * bson_as_canonical_extended_json. Users parse the strings with
 * Amalgame.Formats.Json or any other JSON facade.
 *
 * The reason for JSON-string in/out instead of a typed `BsonValue`
 * tree: keeps the v1 API narrow, avoids inventing yet-another
 * value-tree class, and lets the same wrapper work against any
 * Amalgame JSON parser the user happens to have installed.
 *
 * URI format (libmongoc-standard):
 *   mongodb://[user:pass@]host[:port][/db][?options]
 *   mongodb://127.0.0.1:27017
 *   mongodb://user:pwd@mongo.example.com:27017/mydb?authSource=admin
 *   mongodb+srv://cluster0.foo.mongodb.net      ← Atlas style
 *
 * Threading: AmalgameMongoDB* is single-owner per thread. libmongoc
 * has its own pool API for multi-thread use (mongoc_client_pool_t)
 * — exposing it via Amalgame is a v2 concern.
 *
 * Memory: every internal libmongoc / libbson handle (database,
 * collection, cursor, query bson, json strings) is freed inside
 * the wrapper before each function returns. The top-level client
 * lives for the lifetime of the AmalgameMongoDB* handle and is
 * mongoc_client_destroy()'d in Close(). GC finalizer registered
 * so a leaked handle still releases the client eventually.
 *
 * Init: libmongoc requires a one-time mongoc_init() / mongoc_cleanup()
 * pair per process. The wrapper init is lazy (first Connect()
 * triggers it); cleanup is deferred to process exit via atexit().
 */

#ifndef AMALGAME_DATABASE_NOSQL_MONGODB_H
#define AMALGAME_DATABASE_NOSQL_MONGODB_H

#include "_runtime.h"
#include "Amalgame_Collections.h"

/* Resolve mongoc.h + bson.h across the two common system layouts. */
#if defined(__has_include)
#  if __has_include(<mongoc/mongoc.h>)
#    include <mongoc/mongoc.h>
#    include <bson/bson.h>
#  elif __has_include(<libmongoc-1.0/mongoc/mongoc.h>)
#    include <libmongoc-1.0/mongoc/mongoc.h>
#    include <libbson-1.0/bson/bson.h>
#  else
#    error "mongoc/mongoc.h not found. Install libmongoc-dev / mongo-c-driver-devel / mongo-c-driver."
#  endif
#else
#  include <mongoc/mongoc.h>
#  include <bson/bson.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AmalgameMongoDB {
    mongoc_client_t* client;        /* the libmongoc client; NULL when closed */
    char*            last_error;    /* GC-strdup'd, or NULL */
} AmalgameMongoDB;

/* ── One-time process-global init ───────────────────── */

static int _ammon_inited = 0;
static void _ammon_atexit_cleanup(void) {
    if (_ammon_inited) {
        mongoc_cleanup();
        _ammon_inited = 0;
    }
}
static inline void _ammon_init_once(void) {
    if (!_ammon_inited) {
        mongoc_init();
        atexit(_ammon_atexit_cleanup);
        _ammon_inited = 1;
    }
}

/* ── Helpers ────────────────────────────────────────── */

static inline code_string _ammon_str_dup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static inline code_string _ammon_err_from_bson(const bson_error_t* err) {
    if (!err || !err->message[0]) return _ammon_str_dup("");
    return _ammon_str_dup(err->message);
}

/* Convert a JSON string to a fresh BSON document. Caller frees
 * via bson_destroy(). Returns NULL on parse failure; the caller
 * is expected to set last_error from the bson_error_t. */
static inline bson_t* _ammon_bson_from_json(const char* jsonStr,
                                              bson_error_t* err) {
    if (!jsonStr || !*jsonStr) return bson_new();  /* empty doc */
    return bson_new_from_json((const uint8_t*) jsonStr, -1, err);
}

/* GC finalizer — runs mongoc_client_destroy if the user dropped
 * the handle without calling Close. */
static void _ammon_finalize(void* obj, void* cd) {
    (void) cd;
    AmalgameMongoDB* db = (AmalgameMongoDB*) obj;
    if (db && db->client) {
        mongoc_client_destroy(db->client);
        db->client = NULL;
    }
}

static inline AmalgameMongoDB* _ammon_alloc(void) {
    AmalgameMongoDB* db =
        (AmalgameMongoDB*) GC_MALLOC(sizeof(AmalgameMongoDB));
    db->client     = NULL;
    db->last_error = NULL;
    GC_register_finalizer(db, _ammon_finalize, NULL, NULL, NULL);
    return db;
}

/* ── Lifecycle ──────────────────────────────────────── */

static inline AmalgameMongoDB* Amalgame_Database_NoSQL_MongoDB_Connect(code_string uri) {
    _ammon_init_once();
    AmalgameMongoDB* db = _ammon_alloc();
    if (!uri || !*uri) {
        db->last_error = _ammon_str_dup("null or empty URI");
        return db;
    }
    bson_error_t err;
    mongoc_uri_t* parsed = mongoc_uri_new_with_error(uri, &err);
    if (!parsed) {
        db->last_error = _ammon_err_from_bson(&err);
        return db;
    }
    mongoc_client_t* c = mongoc_client_new_from_uri(parsed);
    mongoc_uri_destroy(parsed);
    if (!c) {
        db->last_error = _ammon_str_dup("mongoc_client_new_from_uri returned NULL");
        return db;
    }
    /* mongoc_client_new* doesn't actually establish a TCP
     * connection — that happens lazily on the first operation.
     * For users who want a "did the network work?" answer
     * immediately, call Ping() right after Connect(). */
    db->client = c;
    return db;
}

static inline void Amalgame_Database_NoSQL_MongoDB_Close(AmalgameMongoDB* db) {
    if (!db || !db->client) return;
    mongoc_client_destroy(db->client);
    db->client = NULL;
}

static inline code_bool Amalgame_Database_NoSQL_MongoDB_IsConnected(AmalgameMongoDB* db) {
    return (db && db->client) ? 1 : 0;
}

static inline code_string Amalgame_Database_NoSQL_MongoDB_LastError(AmalgameMongoDB* db) {
    if (!db || !db->last_error) return (code_string) "";
    return db->last_error;
}

/* Ping the server — runs an admin command. Useful right after
 * Connect to confirm the network actually works. */
static inline code_bool Amalgame_Database_NoSQL_MongoDB_Ping(AmalgameMongoDB* db) {
    if (!db || !db->client) {
        if (db) db->last_error = _ammon_str_dup("not connected");
        return 0;
    }
    bson_t* cmd = BCON_NEW("ping", BCON_INT32(1));
    bson_error_t err;
    bool ok = mongoc_client_command_simple(db->client, "admin", cmd, NULL, NULL, &err);
    bson_destroy(cmd);
    if (!ok) {
        db->last_error = _ammon_err_from_bson(&err);
        return 0;
    }
    db->last_error = _ammon_str_dup("");
    return 1;
}

/* `serverStatus` admin command → pull `version` field out. */
static inline code_string Amalgame_Database_NoSQL_MongoDB_ServerVersion(AmalgameMongoDB* db) {
    if (!db || !db->client) return (code_string) "";
    bson_t* cmd = BCON_NEW("buildInfo", BCON_INT32(1));
    bson_t reply;
    bson_error_t err;
    bool ok = mongoc_client_command_simple(db->client, "admin", cmd, NULL, &reply, &err);
    bson_destroy(cmd);
    if (!ok) {
        bson_destroy(&reply);
        if (db) db->last_error = _ammon_err_from_bson(&err);
        return (code_string) "";
    }
    bson_iter_t iter;
    code_string out = (code_string) "";
    if (bson_iter_init_find(&iter, &reply, "version") && BSON_ITER_HOLDS_UTF8(&iter)) {
        const char* v = bson_iter_utf8(&iter, NULL);
        out = _ammon_str_dup(v);
    }
    bson_destroy(&reply);
    return out;
}

/* ── Internal: get a collection handle (caller destroys) ── */

static inline mongoc_collection_t* _ammon_get_coll(
        AmalgameMongoDB* db, code_string dbName, code_string collName) {
    if (!db || !db->client || !dbName || !collName) return NULL;
    return mongoc_client_get_collection(db->client, dbName, collName);
}

/* ── InsertOne ──────────────────────────────────────── */

static inline code_bool Amalgame_Database_NoSQL_MongoDB_InsertOne(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string jsonDoc) {
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return 0; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return 0; }
    bson_error_t err;
    bson_t* doc = _ammon_bson_from_json(jsonDoc, &err);
    if (!doc) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return 0;
    }
    bool ok = mongoc_collection_insert_one(coll, doc, NULL, NULL, &err);
    bson_destroy(doc);
    mongoc_collection_destroy(coll);
    if (!ok) { db->last_error = _ammon_err_from_bson(&err); return 0; }
    db->last_error = _ammon_str_dup("");
    return 1;
}

/* ── FindOne ────────────────────────────────────────── */

/* Returns the first matching document as a canonical-extended-JSON
 * string, or "" if there's no match. NULL query → match everything. */
static inline code_string Amalgame_Database_NoSQL_MongoDB_FindOne(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string queryJson) {
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return (code_string)""; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return (code_string)""; }
    bson_error_t err;
    bson_t* query = _ammon_bson_from_json(queryJson, &err);
    if (!query) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return (code_string) "";
    }
    bson_t* opts = BCON_NEW("limit", BCON_INT64(1));
    mongoc_cursor_t* cur = mongoc_collection_find_with_opts(coll, query, opts, NULL);
    bson_destroy(opts);
    bson_destroy(query);
    code_string out = (code_string) "";
    const bson_t* doc;
    if (mongoc_cursor_next(cur, &doc)) {
        char* json = bson_as_canonical_extended_json(doc, NULL);
        if (json) {
            out = _ammon_str_dup(json);
            bson_free(json);
        }
    }
    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(coll);
    db->last_error = _ammon_str_dup("");
    return out;
}

/* ── FindMany ───────────────────────────────────────── */

static inline AmalgameList* Amalgame_Database_NoSQL_MongoDB_FindMany(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string queryJson) {
    AmalgameList* out = AmalgameList_new();
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return out; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return out; }
    bson_error_t err;
    bson_t* query = _ammon_bson_from_json(queryJson, &err);
    if (!query) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return out;
    }
    mongoc_cursor_t* cur = mongoc_collection_find_with_opts(coll, query, NULL, NULL);
    bson_destroy(query);
    const bson_t* doc;
    while (mongoc_cursor_next(cur, &doc)) {
        char* json = bson_as_canonical_extended_json(doc, NULL);
        if (json) {
            AmalgameList_add(out, (void*) _ammon_str_dup(json));
            bson_free(json);
        }
    }
    if (mongoc_cursor_error(cur, &err)) {
        db->last_error = _ammon_err_from_bson(&err);
    } else {
        db->last_error = _ammon_str_dup("");
    }
    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(coll);
    return out;
}

/* ── DeleteOne ──────────────────────────────────────── */

static inline code_bool Amalgame_Database_NoSQL_MongoDB_DeleteOne(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string queryJson) {
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return 0; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return 0; }
    bson_error_t err;
    bson_t* query = _ammon_bson_from_json(queryJson, &err);
    if (!query) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return 0;
    }
    bool ok = mongoc_collection_delete_one(coll, query, NULL, NULL, &err);
    bson_destroy(query);
    mongoc_collection_destroy(coll);
    if (!ok) { db->last_error = _ammon_err_from_bson(&err); return 0; }
    db->last_error = _ammon_str_dup("");
    return 1;
}

/* ── UpdateOne ──────────────────────────────────────── */

/* updateJson must use MongoDB's update-operator syntax (`{"$set":...}`).
 * Passing a plain replacement doc raises a server-side error. */
static inline code_bool Amalgame_Database_NoSQL_MongoDB_UpdateOne(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string queryJson, code_string updateJson) {
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return 0; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return 0; }
    bson_error_t err;
    bson_t* query = _ammon_bson_from_json(queryJson, &err);
    if (!query) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return 0;
    }
    bson_t* upd = _ammon_bson_from_json(updateJson, &err);
    if (!upd) {
        bson_destroy(query);
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return 0;
    }
    bool ok = mongoc_collection_update_one(coll, query, upd, NULL, NULL, &err);
    bson_destroy(upd);
    bson_destroy(query);
    mongoc_collection_destroy(coll);
    if (!ok) { db->last_error = _ammon_err_from_bson(&err); return 0; }
    db->last_error = _ammon_str_dup("");
    return 1;
}

/* ── Count ──────────────────────────────────────────── */

static inline i64 Amalgame_Database_NoSQL_MongoDB_Count(
        AmalgameMongoDB* db, code_string dbName, code_string collName,
        code_string queryJson) {
    if (!db || !db->client) { if (db) db->last_error = _ammon_str_dup("not connected"); return -1; }
    mongoc_collection_t* coll = _ammon_get_coll(db, dbName, collName);
    if (!coll) { db->last_error = _ammon_str_dup("get_collection failed"); return -1; }
    bson_error_t err;
    bson_t* query = _ammon_bson_from_json(queryJson, &err);
    if (!query) {
        mongoc_collection_destroy(coll);
        db->last_error = _ammon_err_from_bson(&err);
        return -1;
    }
    int64_t n = mongoc_collection_count_documents(coll, query, NULL, NULL, NULL, &err);
    bson_destroy(query);
    mongoc_collection_destroy(coll);
    if (n < 0) { db->last_error = _ammon_err_from_bson(&err); return -1; }
    db->last_error = _ammon_str_dup("");
    return (i64) n;
}

#endif /* AMALGAME_DATABASE_NOSQL_MONGODB_H */
