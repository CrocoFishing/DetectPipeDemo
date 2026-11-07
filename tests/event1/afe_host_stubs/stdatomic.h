#pragma once
/* The test does not run concurrent workers; retain the production atomic API. */
#include <stdbool.h>
typedef bool atomic_bool;
typedef int atomic_int;
#define atomic_init(object, value) (*(object) = (value))
#define atomic_load(object) (*(object))
#define atomic_store(object, value) (*(object) = (value))
