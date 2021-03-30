/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 * Additional copyrights may follow
 * $HEADER$
 */

/**
 * @file:
 *
 * A simple C-language object-oriented system with single inheritance
 * and ownership-based memory management using a retain/release model.
 *
 * A class consists of a struct and singly-instantiated class
 * descriptor.  The first element of the struct must be the parent
 * class's struct.  The class descriptor must be given a well-known
 * name based upon the class struct name (if the struct is sally_t,
 * the class descriptor should be sally_t_class) and must be
 * statically initialized as discussed below.
 *
 * (a) To define a class
 *
 * In a interface (.h) file, define the class.  The first element
 * should always be the parent class, for example
 * @code
 *   struct sally_t
 *   {
 *     parent_t parent;
 *     void *first_member;
 *     ...
 *   };
 *   typedef struct sally_t sally_t;
 *
 *   PRTE_CLASS_DECLARATION(sally_t);
 * @endcode
 * All classes must have a parent which is also class.
 *
 * In an implementation (.c) file, instantiate a class descriptor for
 * the class like this:
 * @code
 *   PRTE_CLASS_INSTANCE(sally_t, parent_t, sally_construct, sally_destruct);
 * @endcode
 * This macro actually expands to
 * @code
 *   prte_class_t sally_t_class = {
 *     "sally_t",
 *     PRTE_CLASS(parent_t),  // pointer to parent_t_class
 *     sally_construct,
 *     sally_destruct,
 *     0, 0, NULL, NULL,
 *     sizeof ("sally_t")
 *   };
 * @endcode
 * This variable should be declared in the interface (.h) file using
 * the PRTE_CLASS_DECLARATION macro as shown above.
 *
 * sally_construct, and sally_destruct are function pointers to the
 * constructor and destructor for the class and are best defined as
 * static functions in the implementation file.  NULL pointers maybe
 * supplied instead.
 *
 * Other class methods may be added to the struct.
 *
 * (b) Class instantiation: dynamic
 *
 * To create a instance of a class (an object) use PRTE_NEW:
 * @code
 *   sally_t *sally = PRTE_NEW(sally_t);
 * @endcode
 * which allocates memory of sizeof(sally_t) and runs the class's
 * constructors.
 *
 * Use PRTE_RETAIN, PRTE_RELEASE to do reference-count-based
 * memory management:
 * @code
 *   PRTE_RETAIN(sally);
 *   PRTE_RELEASE(sally);
 *   PRTE_RELEASE(sally);
 * @endcode
 * When the reference count reaches zero, the class's destructor, and
 * those of its parents, are run and the memory is freed.
 *
 * N.B. There is no explicit free/delete method for dynamic objects in
 * this model.
 *
 * (c) Class instantiation: static
 *
 * For an object with static (or stack) allocation, it is only
 * necessary to initialize the memory, which is done using
 * PRTE_CONSTRUCT:
 * @code
 *   sally_t sally;
 *
 *   PRTE_CONSTRUCT(&sally, sally_t);
 * @endcode
 * The retain/release model is not necessary here, but before the
 * object goes out of scope, PRTE_DESTRUCT should be run to release
 * initialized resources:
 * @code
 *   PRTE_DESTRUCT(&sally);
 * @endcode
 */

#ifndef PRTE_OBJECT_H
#define PRTE_OBJECT_H

#include "prte_config.h"
#include <assert.h>
#include <stdlib.h>

#include "src/threads/thread_usage.h"

BEGIN_C_DECLS

#if PRTE_ENABLE_DEBUG
/* Any kind of unique ID should do the job */
#    define PRTE_OBJ_MAGIC_ID ((0xdeafbeedULL << 32) + 0xdeafbeedULL)
#endif

/* typedefs ***********************************************************/

typedef struct prte_object_t prte_object_t;
typedef struct prte_class_t prte_class_t;
typedef void (*prte_construct_t)(prte_object_t *);
typedef void (*prte_destruct_t)(prte_object_t *);

/* types **************************************************************/

/**
 * Class descriptor.
 *
 * There should be a single instance of this descriptor for each class
 * definition.
 */
struct prte_class_t {
    const char *cls_name;           /**< symbolic name for class */
    prte_class_t *cls_parent;       /**< parent class descriptor */
    prte_construct_t cls_construct; /**< class constructor */
    prte_destruct_t cls_destruct;   /**< class destructor */
    int cls_initialized;            /**< is class initialized */
    int cls_depth;                  /**< depth of class hierarchy tree */
    prte_construct_t *cls_construct_array;
    /**< array of parent class constructors */
    prte_destruct_t *cls_destruct_array;
    /**< array of parent class destructors */
    size_t cls_sizeof; /**< size of an object instance */
};

PRTE_EXPORT extern int prte_class_init_epoch;

/**
 * For static initializations of OBJects.
 *
 * @param NAME   Name of the class to initialize
 */
#if PRTE_ENABLE_DEBUG
#    define PRTE_OBJ_STATIC_INIT(BASE_CLASS)                                                       \
        {                                                                                          \
            .obj_magic_id = PRTE_OBJ_MAGIC_ID, .obj_class = PRTE_CLASS(BASE_CLASS),                \
            .obj_reference_count = 1, .cls_init_file_name = __FILE__, .cls_init_lineno = __LINE__, \
        }
#else
#    define PRTE_OBJ_STATIC_INIT(BASE_CLASS)                               \
        {                                                                  \
            .obj_class = PRTE_CLASS(BASE_CLASS), .obj_reference_count = 1, \
        }
#endif

/**
 * Base object.
 *
 * This is special and does not follow the pattern for other classes.
 */
struct prte_object_t {
#if PRTE_ENABLE_DEBUG
    /** Magic ID -- want this to be the very first item in the
        struct's memory */
    uint64_t obj_magic_id;
#endif
    prte_class_t *obj_class;                 /**< class descriptor */
    prte_atomic_int32_t obj_reference_count; /**< reference count */
#if PRTE_ENABLE_DEBUG
    const char
        *cls_init_file_name; /**< In debug mode store the file where the object get contructed */
    int cls_init_lineno; /**< In debug mode store the line number where the object get contructed */
#endif                   /* PRTE_ENABLE_DEBUG */
};

/* macros ************************************************************/

/**
 * Return a pointer to the class descriptor associated with a
 * class type.
 *
 * @param NAME          Name of class
 * @return              Pointer to class descriptor
 */
#define PRTE_CLASS(NAME) (&(NAME##_class))

/**
 * Static initializer for a class descriptor
 *
 * @param NAME          Name of class
 * @param PARENT        Name of parent class
 * @param CONSTRUCTOR   Pointer to constructor
 * @param DESTRUCTOR    Pointer to destructor
 *
 * Put this in NAME.c
 */
#define PRTE_CLASS_INSTANCE(NAME, PARENT, CONSTRUCTOR, DESTRUCTOR) \
    prte_class_t NAME##_class = {#NAME,                            \
                                 PRTE_CLASS(PARENT),               \
                                 (prte_construct_t) CONSTRUCTOR,   \
                                 (prte_destruct_t) DESTRUCTOR,     \
                                 0,                                \
                                 0,                                \
                                 NULL,                             \
                                 NULL,                             \
                                 sizeof(NAME)}

/**
 * Declaration for class descriptor
 *
 * @param NAME          Name of class
 *
 * Put this in NAME.h
 */
#define PRTE_CLASS_DECLARATION(NAME) extern prte_class_t NAME##_class

/**
 * Create an object: dynamically allocate storage and run the class
 * constructor.
 *
 * @param type          Type (class) of the object
 * @return              Pointer to the object
 */
static inline prte_object_t *prte_obj_new(prte_class_t *cls);
#if PRTE_ENABLE_DEBUG
static inline prte_object_t *prte_obj_new_debug(prte_class_t *type, const char *file, int line)
{
    prte_object_t *object = prte_obj_new(type);
    object->obj_magic_id = PRTE_OBJ_MAGIC_ID;
    object->cls_init_file_name = file;
    object->cls_init_lineno = line;
    return object;
}
#    define PRTE_NEW(type) ((type *) prte_obj_new_debug(PRTE_CLASS(type), __FILE__, __LINE__))
#else
#    define PRTE_NEW(type) ((type *) prte_obj_new(PRTE_CLASS(type)))
#endif /* PRTE_ENABLE_DEBUG */

/**
 * Retain an object (by incrementing its reference count)
 *
 * @param object        Pointer to the object
 */
#if PRTE_ENABLE_DEBUG
#    define PRTE_RETAIN(object)                                                      \
        do {                                                                         \
            assert(NULL != ((prte_object_t *) (object))->obj_class);                 \
            assert(PRTE_OBJ_MAGIC_ID == ((prte_object_t *) (object))->obj_magic_id); \
            prte_obj_update((prte_object_t *) (object), 1);                          \
            assert(((prte_object_t *) (object))->obj_reference_count >= 0);          \
        } while (0)
#else
#    define PRTE_RETAIN(object) prte_obj_update((prte_object_t *) (object), 1);
#endif

/**
 * Helper macro for the debug mode to store the locations where the status of
 * an object change.
 */
#if PRTE_ENABLE_DEBUG
#    define PRTE_REMEMBER_FILE_AND_LINENO(OBJECT, FILE, LINENO)      \
        do {                                                         \
            ((prte_object_t *) (OBJECT))->cls_init_file_name = FILE; \
            ((prte_object_t *) (OBJECT))->cls_init_lineno = LINENO;  \
        } while (0)
#    define PRTE_SET_MAGIC_ID(OBJECT, VALUE)                      \
        do {                                                      \
            ((prte_object_t *) (OBJECT))->obj_magic_id = (VALUE); \
        } while (0)
#else
#    define PRTE_REMEMBER_FILE_AND_LINENO(OBJECT, FILE, LINENO)
#    define PRTE_SET_MAGIC_ID(OBJECT, VALUE)
#endif /* PRTE_ENABLE_DEBUG */

/**
 * Release an object (by decrementing its reference count).  If the
 * reference count reaches zero, destruct (finalize) the object and
 * free its storage.
 *
 * Note: If the object is freed, then the value of the pointer is set
 * to NULL.
 *
 * @param object        Pointer to the object
 *
 *
 */
#if PRTE_ENABLE_DEBUG
#    define PRTE_RELEASE(object)                                                     \
        do {                                                                         \
            assert(PRTE_OBJ_MAGIC_ID == ((prte_object_t *) (object))->obj_magic_id); \
            assert(NULL != ((prte_object_t *) (object))->obj_class);                 \
            if (0 == prte_obj_update((prte_object_t *) (object), -1)) {              \
                PRTE_SET_MAGIC_ID((object), 0);                                      \
                prte_obj_run_destructors((prte_object_t *) (object));                \
                PRTE_REMEMBER_FILE_AND_LINENO(object, __FILE__, __LINE__);           \
                free(object);                                                        \
                object = NULL;                                                       \
            }                                                                        \
        } while (0)
#else
#    define PRTE_RELEASE(object)                                        \
        do {                                                            \
            if (0 == prte_obj_update((prte_object_t *) (object), -1)) { \
                prte_obj_run_destructors((prte_object_t *) (object));   \
                free(object);                                           \
                object = NULL;                                          \
            }                                                           \
        } while (0)
#endif

/**
 * Construct (initialize) objects that are not dynamically allocated.
 *
 * @param object        Pointer to the object
 * @param type          The object type
 */

#define PRTE_CONSTRUCT(object, type)                         \
    do {                                                     \
        PRTE_CONSTRUCT_INTERNAL((object), PRTE_CLASS(type)); \
    } while (0)

#define PRTE_CONSTRUCT_INTERNAL(object, type)                      \
    do {                                                           \
        PRTE_SET_MAGIC_ID((object), PRTE_OBJ_MAGIC_ID);            \
        if (prte_class_init_epoch != (type)->cls_initialized) {    \
            prte_class_initialize((type));                         \
        }                                                          \
        ((prte_object_t *) (object))->obj_class = (type);          \
        ((prte_object_t *) (object))->obj_reference_count = 1;     \
        prte_obj_run_constructors((prte_object_t *) (object));     \
        PRTE_REMEMBER_FILE_AND_LINENO(object, __FILE__, __LINE__); \
    } while (0)

/**
 * Destruct (finalize) an object that is not dynamically allocated.
 *
 * @param object        Pointer to the object
 */
#if PRTE_ENABLE_DEBUG
#    define PRTE_DESTRUCT(object)                                                    \
        do {                                                                         \
            assert(PRTE_OBJ_MAGIC_ID == ((prte_object_t *) (object))->obj_magic_id); \
            PRTE_SET_MAGIC_ID((object), 0);                                          \
            prte_obj_run_destructors((prte_object_t *) (object));                    \
            PRTE_REMEMBER_FILE_AND_LINENO(object, __FILE__, __LINE__);               \
        } while (0)
#else
#    define PRTE_DESTRUCT(object)                                      \
        do {                                                           \
            prte_obj_run_destructors((prte_object_t *) (object));      \
            PRTE_REMEMBER_FILE_AND_LINENO(object, __FILE__, __LINE__); \
        } while (0)
#endif

PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_object_t);

/* declarations *******************************************************/

/**
 * Lazy initialization of class descriptor.
 *
 * Specifically cache arrays of function pointers for the constructor
 * and destructor hierarchies for this class.
 *
 * @param class    Pointer to class descriptor
 */
PRTE_EXPORT void prte_class_initialize(prte_class_t *);

/**
 * Shut down the class system and release all memory
 *
 * This function should be invoked as the ABSOLUTE LAST function to
 * use the class subsystem.  It frees all associated memory with ALL
 * classes, rendering all of them inoperable.  It is here so that
 * tools like valgrind and purify don't report still-reachable memory
 * upon process termination.
 */
PRTE_EXPORT int prte_class_finalize(void);

/**
 * Run the hierarchy of class constructors for this object, in a
 * parent-first order.
 *
 * Do not use this function directly: use PRTE_CONSTRUCT() instead.
 *
 * WARNING: This implementation relies on a hardwired maximum depth of
 * the inheritance tree!!!
 *
 * Hardwired for fairly shallow inheritance trees
 * @param size          Pointer to the object.
 */
static inline void prte_obj_run_constructors(prte_object_t *object)
{
    prte_construct_t *cls_construct;

    assert(NULL != object->obj_class);

    cls_construct = object->obj_class->cls_construct_array;
    while (NULL != *cls_construct) {
        (*cls_construct)(object);
        cls_construct++;
    }
}

/**
 * Run the hierarchy of class destructors for this object, in a
 * parent-last order.
 *
 * Do not use this function directly: use PRTE_DESTRUCT() instead.
 *
 * @param size          Pointer to the object.
 */
static inline void prte_obj_run_destructors(prte_object_t *object)
{
    prte_destruct_t *cls_destruct;

    assert(NULL != object->obj_class);

    cls_destruct = object->obj_class->cls_destruct_array;
    while (NULL != *cls_destruct) {
        (*cls_destruct)(object);
        cls_destruct++;
    }
}

/**
 * Create new object: dynamically allocate storage and run the class
 * constructor.
 *
 * Do not use this function directly: use PRTE_NEW() instead.
 *
 * @param size          Size of the object
 * @param cls           Pointer to the class descriptor of this object
 * @return              Pointer to the object
 */
static inline prte_object_t *prte_obj_new(prte_class_t *cls)
{
    prte_object_t *object;
    assert(cls->cls_sizeof >= sizeof(prte_object_t));

    object = (prte_object_t *) malloc(cls->cls_sizeof);
    if (prte_class_init_epoch != cls->cls_initialized) {
        prte_class_initialize(cls);
    }
    if (NULL != object) {
        object->obj_class = cls;
        object->obj_reference_count = 1;
        prte_obj_run_constructors(object);
    }
    return object;
}

/**
 * Atomically update the object's reference count by some increment.
 *
 * This function should not be used directly: it is called via the
 * macros PRTE_RETAIN and PRTE_RELEASE
 *
 * @param object        Pointer to the object
 * @param inc           Increment by which to update reference count
 * @return              New value of the reference count
 */
static inline int prte_obj_update(prte_object_t *object, int inc) __prte_attribute_always_inline__;
static inline int prte_obj_update(prte_object_t *object, int inc)
{
    return PRTE_THREAD_ADD_FETCH32(&object->obj_reference_count, inc);
}

END_C_DECLS

#endif
