# amalgame-database-nosql-mongodb

MongoDB binding for [Amalgame](https://github.com/amalgame-lang/Amalgame).
Dynamic-linked to the official **mongo-c-driver** (libmongoc-1.0 +
libbson-1.0). **First amalgame-lang package to link against two
system libraries** and to expose a document model (JSON strings
in/out, BSON under the hood).

## Why dynamic-link

mongo-c-driver is ~150k LoC across both libs, tracks the MongoDB
server protocol, and is universally packaged. Vendoring would
mean ~3 MB binary growth and stale protocol versions; dynamic
linking keeps the wrapper at ~400 LoC and lets the OS handle
version tracking.

## Prerequisites

Install the mongo-c-driver development packages on your build
machine:

| OS / distro | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install libmongoc-dev libbson-dev` |
| Fedora / RHEL / Rocky | `sudo dnf install mongo-c-driver-devel` |
| Arch / Manjaro | `sudo pacman -S mongo-c-driver` |
| Alpine | `apk add mongo-c-driver-dev` |
| macOS (Homebrew) | `brew install mongo-c-driver` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-mongo-c-driver` |
| Windows (vanilla) | install from [mongodb.com/docs/languages/c/c-driver](https://www.mongodb.com/docs/languages/c/c-driver/current/) |

On the **deploy** machine you need the runtime variants:
`libmongoc-1.0` + `libbson-1.0` (often pulled in as deps of
the C driver dev package).

## Install

```bash
amc package add mongodb                                                  # via index
amc package add github.com/amalgame-lang/amalgame-database-nosql-mongodb@v0.1.0
```

Requires **amc 0.8.19+**.

## Surface

```amalgame
import Amalgame.Database.NoSQL.MongoDB

let db = MongoDB.Connect("mongodb://127.0.0.1:27017")
if (!MongoDB.IsConnected(db)) {
    Console.WriteLine("connect failed: " + MongoDB.LastError(db))
    return
}

if (!MongoDB.Ping(db)) {
    Console.WriteLine("server unreachable: " + MongoDB.LastError(db))
    return
}

MongoDB.InsertOne(db, "mydb", "notes", "{\"body\":\"hello mongo\",\"n\":1}")
MongoDB.InsertOne(db, "mydb", "notes", "{\"body\":\"second\",\"n\":2}")

let n: int = MongoDB.Count(db, "mydb", "notes", "{}")
Console.WriteLine("docs: " + String_FromInt(n))

let all = MongoDB.FindMany(db, "mydb", "notes", "{}")
let i: int = 0
while (i < all.Count()) {
    let doc: string = all.Get(i)
    Console.WriteLine(doc)
    i = i + 1
}

MongoDB.UpdateOne(db, "mydb", "notes",
    "{\"n\":1}", "{\"$set\":{\"body\":\"updated\"}}")

MongoDB.DeleteOne(db, "mydb", "notes", "{\"n\":2}")

MongoDB.Close(db)
```

### v0.1.0 method surface

| Method | Returns | Notes |
|---|---|---|
| `MongoDB.Connect(uri)` | `AmalgameMongoDB*` | `mongodb://[user:pass@]host[:port][/db][?opts]` |
| `MongoDB.Close(db)` | `void` | Idempotent; GC finalizer as backup |
| `MongoDB.IsConnected(db)` | `bool` | Client object present; doesn't ping |
| `MongoDB.LastError(db)` | `string` | Empty on success |
| `MongoDB.Ping(db)` | `bool` | `{ping:1}` admin command round-trip |
| `MongoDB.ServerVersion(db)` | `string` | "7.0.5", "6.0.12", … |
| `MongoDB.InsertOne(db, dbName, coll, jsonDoc)` | `bool` | |
| `MongoDB.FindOne(db, dbName, coll, queryJson)` | `string` | JSON doc, `""` if no match |
| `MongoDB.FindMany(db, dbName, coll, queryJson)` | `List<string>` | Each entry is a JSON doc |
| `MongoDB.DeleteOne(db, dbName, coll, queryJson)` | `bool` | |
| `MongoDB.UpdateOne(db, dbName, coll, queryJson, updateJson)` | `bool` | `updateJson` MUST use operator syntax (`{"$set":...}`) |
| `MongoDB.Count(db, dbName, coll, queryJson)` | `int` | `-1` on error |

### Document model

All documents flow as **JSON strings**:
- **In** (Insert / queries) — your JSON is parsed via
  `bson_new_from_json` → BSON before being shipped over the wire.
- **Out** (FindOne / FindMany) — BSON docs are serialised back to
  JSON via `bson_as_canonical_extended_json` for the AM caller.

Parse the returned strings with `Amalgame.Formats.Json` or any
other JSON facade. Native typed accessors (`AsInt(field)` etc.)
on a `BsonValue` tree are v2 work; keeping the v1 surface narrow
means one less concept to learn before you can do useful CRUD.

### URI examples

```
mongodb://127.0.0.1:27017                          // localhost, no auth
mongodb://user:pwd@mongo.example.com:27017/mydb    // auth + default db
mongodb://user:pwd@host/mydb?authSource=admin      // explicit authSource
mongodb+srv://cluster0.foo.mongodb.net             // Atlas SRV record
```

Full URI spec: [MongoDB connection string](https://www.mongodb.com/docs/manual/reference/connection-string/).

## Deferred to v2

- Aggregation pipelines (`Aggregate(coll, pipelineJson)`)
- Indexes (`CreateIndex`, `ListIndexes`, `DropIndex`)
- Change streams (`Watch(coll, pipelineJson) → cursor`)
- Bulk write (`InsertMany`, `UpdateMany`, `DeleteMany` batches)
- Transactions (`StartSession` / `CommitTransaction`)
- GridFS (large blob storage)
- Native typed accessors (vs the current JSON-string round-trip)
- Connection pooling (`mongoc_client_pool_t`) for multi-thread apps

## Threading

`AmalgameMongoDB*` is single-owner per thread. libmongoc provides
its own pool (`mongoc_client_pool_t`) for multi-thread use; that
surface lands in v2. Concurrent ops against the same handle from
different threads are unsafe.

## Tests

```bash
./tests/run_tests.sh /path/to/amc
```

Double-gated runner. Both `libmongoc-dev` AND a reachable MongoDB
on `127.0.0.1:27017` (or `$MONGO_URI`) must be present, else
every case SKIPs cleanly. Start a server locally with:

```bash
docker run --rm -d --name mongotest -p 27017:27017 mongo:7
```

then export `MONGO_URI=mongodb://127.0.0.1:27017` (this is the
default if unset) before running the suite.

## Licence

Apache-2.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).
mongo-c-driver itself is Apache-2.0 too — directly compatible.
