/* zconf.h -- configuration of the zlib compression library
 * Copyright (C) 1995-2005 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifndef ZCONF_H
#define ZCONF_H

/*
 * If you *really* need a unique prefix for all types and library functions,
 * compile with -DZ_PREFIX. bx changes this prefix to bx_rsync_z_ and also
 * renames internal symbols so this copy can coexist with the system zlib.
 * The "standard" zlib should be compiled without it.
 */
#ifdef Z_PREFIX
#  define deflateInit_          bx_rsync_z_deflateInit_
#  define deflate               bx_rsync_z_deflate
#  define deflateEnd            bx_rsync_z_deflateEnd
#  define inflateInit_          bx_rsync_z_inflateInit_
#  define inflate               bx_rsync_z_inflate
#  define inflateEnd            bx_rsync_z_inflateEnd
#  define deflateInit2_         bx_rsync_z_deflateInit2_
#  define deflateSetDictionary  bx_rsync_z_deflateSetDictionary
#  define deflateCopy           bx_rsync_z_deflateCopy
#  define deflateReset          bx_rsync_z_deflateReset
#  define deflateParams         bx_rsync_z_deflateParams
#  define deflateBound          bx_rsync_z_deflateBound
#  define deflatePrime          bx_rsync_z_deflatePrime
#  define inflateInit2_         bx_rsync_z_inflateInit2_
#  define inflateSetDictionary  bx_rsync_z_inflateSetDictionary
#  define inflateSync           bx_rsync_z_inflateSync
#  define inflateSyncPoint      bx_rsync_z_inflateSyncPoint
#  define inflateCopy           bx_rsync_z_inflateCopy
#  define inflateReset          bx_rsync_z_inflateReset
#  define inflateBack           bx_rsync_z_inflateBack
#  define inflateBackEnd        bx_rsync_z_inflateBackEnd
#  define compress2             bx_rsync_z_compress2
#  define compressBound         bx_rsync_z_compressBound
#  define uncompress            bx_rsync_z_uncompress
#  define adler32               bx_rsync_z_adler32
#  define crc32                 bx_rsync_z_crc32
#  define get_crc_table         bx_rsync_z_get_crc_table
#  define zError                bx_rsync_z_zError

#  define alloc_func            bx_rsync_z_alloc_func
#  define free_func             bx_rsync_z_free_func
#  define in_func               bx_rsync_z_in_func
#  define out_func              bx_rsync_z_out_func
#  define Byte                  bx_rsync_z_Byte
#  define uInt                  bx_rsync_z_uInt
#  define uLong                 bx_rsync_z_uLong
#  define Bytef                 bx_rsync_z_Bytef
#  define charf                 bx_rsync_z_charf
#  define intf                  bx_rsync_z_intf
#  define uIntf                 bx_rsync_z_uIntf
#  define uLongf                bx_rsync_z_uLongf
#  define voidpf                bx_rsync_z_voidpf
#  define voidp                 bx_rsync_z_voidp
#  define _dist_code            bx_rsync_z_dist_code
#  define _length_code          bx_rsync_z_length_code
#  define _tr_align             bx_rsync_z_tr_align
#  define _tr_flush_block       bx_rsync_z_tr_flush_block
#  define _tr_init              bx_rsync_z_tr_init
#  define _tr_stored_block      bx_rsync_z_tr_stored_block
#  define _tr_tally             bx_rsync_z_tr_tally
#  define adler32_combine       bx_rsync_z_adler32_combine
#  define crc32_combine         bx_rsync_z_crc32_combine
#  define deflateSetHeader      bx_rsync_z_deflateSetHeader
#  define deflateTune           bx_rsync_z_deflateTune
#  define deflate_copyright     bx_rsync_z_deflate_copyright
#  define inflateGetHeader      bx_rsync_z_inflateGetHeader
#  define inflatePrime          bx_rsync_z_inflatePrime
#  define inflate_copyright     bx_rsync_z_inflate_copyright
#  define inflate_fast          bx_rsync_z_inflate_fast
#  define inflate_table         bx_rsync_z_inflate_table
#  define zcalloc               bx_rsync_z_zcalloc
#  define zcfree                bx_rsync_z_zcfree
#  define z_errmsg              bx_rsync_z_errmsg
#  define zlibCompileFlags      bx_rsync_z_zlibCompileFlags
#  define zlibVersion           bx_rsync_z_zlibVersion

#endif

#if defined(__MSDOS__) && !defined(MSDOS)
#  define MSDOS
#endif
#if (defined(OS_2) || defined(__OS2__)) && !defined(OS2)
#  define OS2
#endif
#if defined(_WINDOWS) && !defined(WINDOWS)
#  define WINDOWS
#endif
#if defined(_WIN32) || defined(_WIN32_WCE) || defined(__WIN32__)
#  ifndef WIN32
#    define WIN32
#  endif
#endif
#if (defined(MSDOS) || defined(OS2) || defined(WINDOWS)) && !defined(WIN32)
#  if !defined(__GNUC__) && !defined(__FLAT__) && !defined(__386__)
#    ifndef SYS16BIT
#      define SYS16BIT
#    endif
#  endif
#endif

/*
 * Compile with -DMAXSEG_64K if the alloc function cannot allocate more
 * than 64k bytes at a time (needed on systems with 16-bit int).
 */
#ifdef SYS16BIT
#  define MAXSEG_64K
#endif
#ifdef MSDOS
#  define UNALIGNED_OK
#endif

#ifdef __STDC_VERSION__
#  ifndef STDC
#    define STDC
#  endif
#  if __STDC_VERSION__ >= 199901L
#    ifndef STDC99
#      define STDC99
#    endif
#  endif
#endif
#if !defined(STDC) && (defined(__STDC__) || defined(__cplusplus))
#  define STDC
#endif
#if !defined(STDC) && (defined(__GNUC__) || defined(__BORLANDC__))
#  define STDC
#endif
#if !defined(STDC) && (defined(MSDOS) || defined(WINDOWS) || defined(WIN32))
#  define STDC
#endif
#if !defined(STDC) && (defined(OS2) || defined(__HOS_AIX__))
#  define STDC
#endif

#if defined(__OS400__) && !defined(STDC)    /* iSeries (formerly AS/400). */
#  define STDC
#endif

#ifndef STDC
#  ifndef const /* cannot use !defined(STDC) && !defined(const) on Mac */
#    define const       /* note: need a more gentle solution here */
#  endif
#endif

/* Some Mac compilers merge all .h files incorrectly: */
#if defined(__MWERKS__)||defined(applec)||defined(THINK_C)||defined(__SC__)
#  define NO_DUMMY_DECL
#endif

/* Maximum value for memLevel in deflateInit2 */
#ifndef MAX_MEM_LEVEL
#  ifdef MAXSEG_64K
#    define MAX_MEM_LEVEL 8
#  else
#    define MAX_MEM_LEVEL 9
#  endif
#endif

/* Maximum value for windowBits in deflateInit2 and inflateInit2.
 * WARNING: reducing MAX_WBITS makes minigzip unable to extract .gz files
 * created by gzip. (Files created by minigzip can still be extracted by
 * gzip.)
 */
#ifndef MAX_WBITS
#  define MAX_WBITS   15 /* 32K LZ77 window */
#endif

/* The memory requirements for deflate are (in bytes):
            (1 << (windowBits+2)) +  (1 << (memLevel+9))
 that is: 128K for windowBits=15  +  128K for memLevel = 8  (default values)
 plus a few kilobytes for small objects. For example, if you want to reduce
 the default memory requirements from 256K to 128K, compile with
     make CFLAGS="-O -DMAX_WBITS=14 -DMAX_MEM_LEVEL=7"
 Of course this will generally degrade compression (there's no free lunch).

   The memory requirements for inflate are (in bytes) 1 << windowBits
 that is, 32K for windowBits=15 (default value) plus a few kilobytes
 for small objects.
*/

                        /* Type declarations */

#ifndef OF /* function prototypes */
#  ifdef STDC
#    define OF(args)  args
#  else
#    define OF(args)  ()
#  endif
#endif

/* The following definitions for FAR are needed only for MSDOS mixed
 * model programming (small or medium model with some far allocations).
 * This was tested only with MSC; for other MSDOS compilers you may have
 * to define NO_MEMCPY in zutil.h.  If you don't need the mixed model,
 * just define FAR to be empty.
 */
#ifdef SYS16BIT
#  if defined(M_I86SM) || defined(M_I86MM)
     /* MSC small or medium model */
#    define SMALL_MEDIUM
#    ifdef _MSC_VER
#      define FAR _far
#    else
#      define FAR far
#    endif
#  endif
#  if (defined(__SMALL__) || defined(__MEDIUM__))
     /* Turbo C small or medium model */
#    define SMALL_MEDIUM
#    ifdef __BORLANDC__
#      define FAR _far
#    else
#      define FAR far
#    endif
#  endif
#endif

#if defined(WINDOWS) || defined(WIN32)
   /* If building or using zlib as a DLL, define ZLIB_DLL.
    * This is not mandatory, but it offers a little performance increase.
    */
#  ifdef ZLIB_DLL
#    if defined(WIN32) && (!defined(__BORLANDC__) || (__BORLANDC__ >= 0x500))
#      ifdef ZLIB_INTERNAL
#        define ZEXTERN extern __declspec(dllexport)
#      else
#        define ZEXTERN extern __declspec(dllimport)
#      endif
#    endif
#  endif  /* ZLIB_DLL */
   /* If building or using zlib with the WINAPI/WINAPIV calling convention,
    * define ZLIB_WINAPI.
    * Caution: the standard ZLIB1.DLL is NOT compiled using ZLIB_WINAPI.
    */
#  ifdef ZLIB_WINAPI
#    ifdef FAR
#      undef FAR
#    endif
#    include <windows.h>
     /* No need for _export, use ZLIB.DEF instead. */
     /* For complete Windows compatibility, use WINAPI, not __stdcall. */
#    define ZEXPORT WINAPI
#    ifdef WIN32
#      define ZEXPORTVA WINAPIV
#    else
#      define ZEXPORTVA FAR CDECL
#    endif
#  endif
#endif

#if defined (__BEOS__)
#  ifdef ZLIB_DLL
#    ifdef ZLIB_INTERNAL
#      define ZEXPORT   __declspec(dllexport)
#      define ZEXPORTVA __declspec(dllexport)
#    else
#      define ZEXPORT   __declspec(dllimport)
#      define ZEXPORTVA __declspec(dllimport)
#    endif
#  endif
#endif

#ifndef ZEXTERN
#  define ZEXTERN extern
#endif
#ifndef ZEXPORT
#  define ZEXPORT
#endif
#ifndef ZEXPORTVA
#  define ZEXPORTVA
#endif

#ifndef FAR
#  define FAR
#endif

#if !defined(__MACTYPES__)
typedef unsigned char  Byte;  /* 8 bits */
#endif
typedef unsigned int   uInt;  /* 16 bits or more */
typedef unsigned long  uLong; /* 32 bits or more */

#ifdef SMALL_MEDIUM
   /* Borland C/C++ and some old MSC versions ignore FAR inside typedef */
#  define Bytef Byte FAR
#else
   typedef Byte  FAR Bytef;
#endif
typedef char  FAR charf;
typedef int   FAR intf;
typedef uInt  FAR uIntf;
typedef uLong FAR uLongf;

#ifdef STDC
   typedef void const *voidpc;
   typedef void FAR   *voidpf;
   typedef void       *voidp;
#else
   typedef Byte const *voidpc;
   typedef Byte FAR   *voidpf;
   typedef Byte       *voidp;
#endif

#if 0           /* HAVE_UNISTD_H -- this line is updated by ./configure */
#  include <sys/types.h> /* for off_t */
#  include <unistd.h>    /* for SEEK_* and off_t */
#  ifdef VMS
#    include <unixio.h>   /* for off_t */
#  endif
#  define z_off_t off_t
#endif
#ifndef SEEK_SET
#  define SEEK_SET        0       /* Seek from beginning of file.  */
#  define SEEK_CUR        1       /* Seek from current position.  */
#  define SEEK_END        2       /* Set file pointer to EOF plus "offset" */
#endif
#ifndef z_off_t
#  define z_off_t long
#endif

#if defined(__OS400__)
#  define NO_vsnprintf
#endif

#if defined(__MVS__)
#  define NO_vsnprintf
#  ifdef FAR
#    undef FAR
#  endif
#endif

/* MVS linker does not support external names larger than 8 bytes */
#if defined(__MVS__)
#   pragma map(deflateInit_,"DEIN")
#   pragma map(deflateInit2_,"DEIN2")
#   pragma map(deflateEnd,"DEEND")
#   pragma map(deflateBound,"DEBND")
#   pragma map(inflateInit_,"ININ")
#   pragma map(inflateInit2_,"ININ2")
#   pragma map(inflateEnd,"INEND")
#   pragma map(inflateSync,"INSY")
#   pragma map(inflateSetDictionary,"INSEDI")
#   pragma map(compressBound,"CMBND")
#   pragma map(inflate_table,"INTABL")
#   pragma map(inflate_fast,"INFA")
#   pragma map(inflate_copyright,"INCOPY")
#endif

#endif /* ZCONF_H */
