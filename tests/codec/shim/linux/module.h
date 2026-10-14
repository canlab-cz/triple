#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#define printk printf
#define KERN_DEBUG ""
/* kernel types referenced by the adapter struct in triple_helper.h */
typedef int spinlock_t;
typedef struct { int counter; } atomic_t;
struct work_struct { int unused; };
struct tty_struct;
struct net_device;
