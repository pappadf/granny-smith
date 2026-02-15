# Granny Smith Project Style Guide

This document describes the coding, formatting, and documentation conventions for the Granny Smith project. All contributors should follow these guidelines to ensure consistency and readability.

## C Source Code

### General Formatting

- Use the formatting rules specified in `.clang-format`

### Naming Conventions

- Use `snake_case` for identifiers: `parse_header()`, `file_info`.
- Constants and macros are `ALL_CAPS_WITH_UNDERSCORES`.
- Prefix internal/private functions with `static` and, if needed, a module prefix: `static int hqx_decode_byte(...)`.

### Comments

- Use `//` for short, inline comments.
- Keep comments up-to-date and relevant.
- For every significant statement or calculation, add a concise one-line comment explaining the purpose or reasoning behind the code—not just restating what the code does, but *why* it is done. This helps future readers understand the intent.
  - Example:
    ```c
    static int sum_positive(int *arr, int n) {
        int sum = 0;
        for (int i = 0; i < n; ++i) {
            // Only add positive numbers to the sum
            if (arr[i] > 0)
                sum += arr[i];
        }
        return sum;
    }
    ```
- Every function should have a one-line comment immediately above it describing its overall purpose or effect, not just repeating the function name or signature. Example:
    ```c
    // Copies up to 'cap' bytes from 'src' to 'dst'
    static size_t copy_bytes(const uint8_t *src, uint8_t *dst, size_t cap) {
        size_t copied = 0;
        while (copied < cap && src[copied]) {
            dst[copied] = src[copied];
            copied++;
        }
        return copied;
    }
    ```
- Every structure definition should have a one-line comment immediately above it describing its purpose or what it represents. Example:
    ```c
    // Holds configuration options for the parser
    struct parser_config {
        int max_depth;
        bool strict_mode;
    };
    ```
- Leave at least one completely empty/blank line between consecutive function and struct declarations. This improves visual separation and makes it easier to spot declaration boundaries. For example:

  **Incorrect** (no blank line between declarations):
  ```c
  // Frees resources associated with a keyboard instance
  void keyboard_delete(keyboard_t* keyboard);
  // Saves keyboard state to a checkpoint
  void keyboard_checkpoint(keyboard_t *restrict keyboard, checkpoint_t *checkpoint);
  ```

  **Correct** (blank line between declarations):
  ```c
  // Frees resources associated with a keyboard instance
  void keyboard_delete(keyboard_t* keyboard);

  // Saves keyboard state to a checkpoint
  void keyboard_checkpoint(keyboard_t *restrict keyboard, checkpoint_t *checkpoint);
  ```

- In normal `.c` files (excluding *public* headers), prefer `//` for one-line and inline comments rather than `/* ... */`. Reserve `/* ... */` for multi-line comments or the file license/header block; public headers may still use Doxygen-style block comments for API documentation.

- **In general, do not remove or change existing comments, unless they are clearly wrong, or if you are fixing language errors or formatting.**

### File Structure

- Every source file must begin with a two-line copyright notice at the very top:
    ```c
    // SPDX-License-Identifier: MIT
    // Copyright (c) pappadf
    ```
  The `SPDX-License-Identifier` tag is the machine-readable standard for license identification. The full MIT license text lives in the repository root `LICENSE` file — do **not** duplicate it in individual file headers.
- Each file starts with a brief file-level comment describing its purpose. Example:
    ```c
    // foo.c
    // Implements the Foo module for Granny Smith.
    ```
- Include guards in all header files. Example:
    ```c
    #ifndef FOO_H
    #define FOO_H

    // ...header contents...

    #endif // FOO_H
    ```

#### Include Ordering

Includes should be ordered consistently to ensure headers are self-contained:

1. **Own header** first (validates that the header is self-contained):
   ```c
   #include "module.h"
   ```
2. **Project headers** (alphabetical order):
   ```c
   #include "common.h"
   #include "log.h"
   #include "system.h"
   ```
3. **System headers** (alphabetical order):
   ```c
   #include <assert.h>
   #include <stddef.h>
   #include <stdlib.h>
   #include <string.h>
   ```

#### Section Comments

Use section comments to organize code into logical groups. Use the following format:

```c
// ============================================================================
// Section Name
// ============================================================================
```

Standard sections for header files (in order):
1. Includes
2. Forward Declarations (if needed)
3. Constants
4. Macros
5. Type Definitions
6. Lifecycle (Constructor / Destructor / Checkpoint)
7. Operations

Standard sections for implementation files (in order):
1. Includes
2. Constants and Macros
3. Type Definitions (Private)
4. Forward Declarations
5. Static Helpers
6. Memory Interface (if applicable)
7. Shell Commands (if applicable)
8. Lifecycle: Constructor
9. Lifecycle: Destructor
10. Lifecycle: Checkpointing
11. Operations (Public API)

Example header section comment:
```c
// === Includes ===
```

Example implementation section comment:
```c
// ============================================================================
// Static Helpers
// ============================================================================
```

#### Function Ordering in Implementation Files

- **Static helper functions** should appear BEFORE lifecycle functions
- **Memory interface callbacks** (read_uint8, write_uint8, etc.) should be grouped together
- **Lifecycle functions** should be ordered: `init` → `delete` → `checkpoint`
- **Public operations** should appear AFTER lifecycle functions

### Other Conventions

- Avoid magic numbers; use named constants.
- Use `bool`, `true`, `false` from `<stdbool.h>`.
- Free all allocated memory; avoid leaks.

---

## Markdown Documentation

### <a name="md-general-formatting"></a>General Formatting

- Use [CommonMark](https://commonmark.org/) compliant Markdown.
- Limit lines to 120 characters for readability.
- Use spaces, not tabs, for indentation.

### Headings

- Use `#` for top-level, `##` for sections, `###` for subsections.
- Leave a blank line before and after headings.

### Lists and Code Blocks

- Use `-` or `*` for unordered lists, `1.` for ordered lists.
- Indent code blocks with triple backticks and specify language when possible:
  ````markdown
  ```c
  int main(void) { return 0; }
  ```
````
