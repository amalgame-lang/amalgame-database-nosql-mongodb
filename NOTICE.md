# NOTICE — amalgame-database-nosql-mongodb

## Authorship

Copyright 2026 Bastien Mouget. Original work — see
`runtime/Amalgame_Database_NoSQL_MongoDB.h`.

Part of the Amalgame ecosystem
([github.com/amalgame-lang/Amalgame](https://github.com/amalgame-lang/Amalgame)).
External contributions are paused at the ecosystem level; see the
main repo's `CONTRIBUTING.md` for the policy.

AI tools (Anthropic Claude) were used during development. Per
the project's authorship policy, AI is treated as a tool, not a
co-author at law.

## Licence

Apache License 2.0. See `LICENSE` for the full text.

## Third-party content

**None vendored.** This package binds to two system libraries:

### libmongoc — MongoDB Inc. + contributors

`libmongoc-1.0`, the official MongoDB C client. Distributed under
the [Apache License 2.0](https://github.com/mongodb/mongo-c-driver/blob/master/COPYING)
— directly compatible with this package's licence.

### libbson — MongoDB Inc. + contributors

`libbson-1.0`, the BSON serialisation / parsing library that ships
with mongo-c-driver. Distributed under the
[Apache License 2.0](https://github.com/mongodb/mongo-c-driver/blob/master/COPYING)
— directly compatible with this package's licence.

Both libraries are provided by the user's operating system at
compile time (via `libmongoc-dev` / `mongo-c-driver-devel` /
`mongo-c-driver`) and at run time (via `libmongoc-1.0` /
`libbson-1.0`).

This package does not include or redistribute any libmongoc or
libbson code; users obtain those independently from their OS
package manager or from mongodb.com.

## Trademarks

"MongoDB" is a trademark of MongoDB Inc. This repository uses
the name solely to identify the database the package binds to.
No trademark claim is asserted.
