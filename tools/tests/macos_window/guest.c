/*
GUEST.C

A guest for tools/test_macos_asm_rebase.py: what the rebased code must keep
working - lists of pointers, pointers compared (globals', loaded ones, the
stack's), function pointer tables, jump tables, variable-length arrays,
vectorized loops, variadic arguments and atomics - each result handed to the
host through an import stub, whose pointer argument the stub rebases.
*/

typedef unsigned int u32;
typedef __SIZE_TYPE__ size_t;
#include "imports.h"
struct node { int value; struct node *next; char name[12]; };
struct node nodes[8];
struct node *head;
static const char *names[] = { "zero", "one", "two", "three", "four", "five", "six", "seven" };
static int counter;
int (*ops[4])(int, int);
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int dvd(int a, int b) { return b ? a / b : 0; }
__attribute__((noinline)) int pick(int i) { switch (i) { case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55; case 5: return 66; case 6: return 77; default: return -1; } }
__attribute__((noinline)) int vla_sum(int n) { int a[n]; int i, s = 0; for (i = 0; i < n; i++) a[i] = i * 3; for (i = 0; i < n; i++) s += a[i]; return s; }
__attribute__((noinline)) void copy(char *d, const char *s, size_t n) { size_t i; for (i = 0; i < n; i++) d[i] = s[i]; }
__attribute__((noinline)) float dot(const float *a, const float *b, int n) { float s = 0; int i; for (i = 0; i < n; i++) s += a[i] * b[i]; return s; }
__attribute__((noinline)) int is_head(struct node *p) { return p == &nodes[0]; }
__attribute__((noinline)) int is_local(int *p, int *q) { return p == q; }
__attribute__((noinline)) int sum_va(int n, ...) { __builtin_va_list ap; int s = 0; __builtin_va_start(ap, n); while (n--) s += __builtin_va_arg(ap, int); __builtin_va_end(ap); return s; }
float fa[64], fb[64]; char big1[300], big2[300];
int guest_main(void)
{
	int i, local = 5; int *lp = &local; struct node *p; char buf[64];
	ops[0] = add; ops[1] = sub; ops[2] = mul; ops[3] = dvd;
	for (i = 0; i < 8; i++) { nodes[i].value = i * 10; nodes[i].next = i < 7 ? &nodes[i + 1] : 0; copy(nodes[i].name, names[i], 6); }
	head = &nodes[0];
	for (p = head, i = 0; p; p = p->next) i += p->value;
	host_print("list sum (280)", i);
	host_print(head->next->next->name, 0);
	host_print("ops (7,3,30,3)", ops[0](5,2) * 1000 + ops[1](5,2) * 100 + ops[2](5,6) + ops[3](9,3) * 100000);
	host_print("pick (66)", pick(5));
	host_print("vla (4950*3=14850)", vla_sum(100));
	for (i = 0; i < 64; i++) { fa[i] = i; fb[i] = 2; }
	host_print("dot (4032)", (int)dot(fa, fb, 64));
	for (i = 0; i < 300; i++) big1[i] = (char)i; copy(big2, big1, 300);
	host_print("copy (299%256=43)", big2[299]);
	host_print("is_head (1)", is_head(head));
	host_print("is_local (1)", is_local(lp, &local));
	copy(buf, "stack string", 13); host_print(buf, 0);
	host_print("va (15)", sum_va(5, 1, 2, 3, 4, 5));
	__atomic_fetch_add(&counter, 5, __ATOMIC_SEQ_CST);
	host_print("atomic (5)", counter);
	host_print("ptr bits (stack < 4GB: 1)", ((unsigned long long)(u32)lp) == (unsigned long long)(size_t)lp);
	return 42;
}
