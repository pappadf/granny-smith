// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs_class.h
// Directory-level methods of the `files` node, implemented over the VFS in
// vfs_class.c and listed in the `files` member table (storage_class.c).

#ifndef GS_VFS_CLASS_H
#define GS_VFS_CLASS_H

#include "object.h"

value_t files_method_ls(struct object *self, const member_t *m, int argc, const value_t *argv);
value_t files_method_list(struct object *self, const member_t *m, int argc, const value_t *argv);
value_t files_method_mkdir(struct object *self, const member_t *m, int argc, const value_t *argv);
value_t files_method_cat(struct object *self, const member_t *m, int argc, const value_t *argv);
value_t files_method_cd(struct object *self, const member_t *m, int argc, const value_t *argv);
value_t files_method_pwd(struct object *self, const member_t *m, int argc, const value_t *argv);

#endif // GS_VFS_CLASS_H
