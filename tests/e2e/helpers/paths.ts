// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Where the specs find the test data (tests/data, from gs-test-data; see
// scripts/fetch-test-data.sh).  One copy, so a spec in a subdirectory of
// web2-specs resolves the same tree as one at the top.

import * as path from "node:path";

// The test-data root.
export const DATA = path.resolve(__dirname, "../../data");
// ROM images.
export const ROMS = path.join(DATA, "roms");
