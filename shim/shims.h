/*
 * shims.h - the libSystem layer: what a Mach-O's imports bind to.
 *
 * Symbols are looked up by their Mach-O name, i.e. with the leading underscore
 * that C symbols get on Apple platforms ("_puts", not "puts").
 *
 * Anything we do not provide is bound to a "loud" thunk: calling it prints a
 * TRACE-format line and aborts, so a missing function is never a silent no-op.
 */
#ifndef IDROID_SHIMS_H
#define IDROID_SHIMS_H

/* Address of the shim for `name`, or NULL if there is none. */
void *shim_lookup(const char *name);

/* A thunk that reports `name` as missing when called. NULL if the pool is full.
 * `name` must stay valid for the life of the process. Repeated calls with the
 * same name return the same thunk. */
void *shim_make_unresolved(const char *name);

/* Make the thunk pool executable. Call after loading, before running guest code. */
int shim_seal(void);

/* Where thunks land. Prints "- MISSING symbol: <name>" and aborts. */
void shim_unresolved_hit(const char *name) __attribute__((noreturn));

#endif
