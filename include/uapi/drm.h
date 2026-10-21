/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef UAPI_DRM_H
#define UAPI_DRM_H

/*
 * The DRM/KMS user interface: the ioctls and structs of /dev/dri/card0,
 * with Linux's numbers and layouts (include/uapi/drm/drm.h and drm_mode.h)
 * so that unmodified libdrm, wlroots and modetest work. Only what MyOS
 * implements is here (drivers/gpu/drm/drm_ioctl.c says which and why).
 *
 * Every struct is made of fixed-size fields except drm_version, whose
 * size_t and pointers are 4 bytes on i386 and 8 on x86_64: the kernel is
 * built for the same width as its programs, so the plain C types match.
 */
#include <stddef.h>
#include <stdint.h>

#define DRM_IOCTL_BASE 'd'

/* The ioctl numbers (the low byte of the command). */
enum {
    DRM_NR_VERSION = 0x00, DRM_NR_GET_UNIQUE = 0x01, DRM_NR_GET_MAGIC = 0x02,
    DRM_NR_SET_VERSION = 0x07, DRM_NR_GEM_CLOSE = 0x09,
    DRM_NR_GET_CAP = 0x0c, DRM_NR_SET_CLIENT_CAP = 0x0d, DRM_NR_AUTH_MAGIC = 0x11,
    DRM_NR_SET_MASTER = 0x1e, DRM_NR_DROP_MASTER = 0x1f,
    DRM_NR_PRIME_HANDLE_TO_FD = 0x2d, DRM_NR_PRIME_FD_TO_HANDLE = 0x2e,
    DRM_NR_WAIT_VBLANK = 0x3a,
    DRM_NR_MODE_GETRESOURCES = 0xA0, DRM_NR_MODE_GETCRTC = 0xA1, DRM_NR_MODE_SETCRTC = 0xA2,
    DRM_NR_MODE_CURSOR = 0xA3, DRM_NR_MODE_GETGAMMA = 0xA4, DRM_NR_MODE_SETGAMMA = 0xA5,
    DRM_NR_MODE_GETENCODER = 0xA6, DRM_NR_MODE_GETCONNECTOR = 0xA7,
    DRM_NR_MODE_GETPROPERTY = 0xAA, DRM_NR_MODE_SETPROPERTY = 0xAB,
    DRM_NR_MODE_GETPROPBLOB = 0xAC, DRM_NR_MODE_GETFB = 0xAD, DRM_NR_MODE_ADDFB = 0xAE,
    DRM_NR_MODE_RMFB = 0xAF, DRM_NR_MODE_PAGE_FLIP = 0xB0, DRM_NR_MODE_DIRTYFB = 0xB1,
    DRM_NR_MODE_CREATE_DUMB = 0xB2, DRM_NR_MODE_MAP_DUMB = 0xB3, DRM_NR_MODE_DESTROY_DUMB = 0xB4,
    DRM_NR_MODE_GETPLANERESOURCES = 0xB5, DRM_NR_MODE_GETPLANE = 0xB6,
    DRM_NR_MODE_ADDFB2 = 0xB8, DRM_NR_MODE_OBJ_GETPROPERTIES = 0xB9,
    DRM_NR_MODE_OBJ_SETPROPERTY = 0xBA, DRM_NR_MODE_CURSOR2 = 0xBB, DRM_NR_MODE_ATOMIC = 0xBC,
    DRM_NR_MODE_CREATEPROPBLOB = 0xBD, DRM_NR_MODE_DESTROYPROPBLOB = 0xBE,
    DRM_NR_MODE_CREATE_LEASE = 0xC6, DRM_NR_MODE_LIST_LESSEES = 0xC7,
    DRM_NR_MODE_GETFB2 = 0xCE,
};

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_auth { uint32_t magic; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_set_client_cap { uint64_t capability, value; };
struct drm_set_version { int di_major, di_minor, dd_major, dd_minor; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };

#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define  DRM_PRIME_CAP_IMPORT        0x1
#define  DRM_PRIME_CAP_EXPORT        0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14

#define DRM_CLIENT_CAP_STEREO_3D        1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_CLIENT_CAP_ASPECT_RATIO     4

#define DRM_CLOEXEC 0x80000             /* O_CLOEXEC */
#define DRM_RDWR    0x2                 /* O_RDWR */

/* Read from the file: one of these per finished page flip. */
struct drm_event { uint32_t type, length; };
#define DRM_EVENT_VBLANK        0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02
struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

/* --- modes ------------------------------------------------------------- */

#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN    32

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};

#define DRM_MODE_CONNECTED        1
#define DRM_MODE_SUBPIXEL_UNKNOWN 1
#define DRM_MODE_ENCODER_NONE     0
#define DRM_MODE_ENCODER_VIRTUAL  5
#define DRM_MODE_CONNECTOR_Unknown 0

#define DRM_MODE_PROP_RANGE     (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
#define DRM_MODE_PROP_ENUM      (1 << 3)

struct drm_mode_property_enum {
    uint64_t value;
    char name[DRM_PROP_NAME_LEN];
};

struct drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values, count_enum_blobs;
};

struct drm_mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id, connector_id;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type;
};

struct drm_mode_obj_set_property {
    uint64_t value;
    uint32_t prop_id, obj_id, obj_type;
};

struct drm_mode_get_blob { uint32_t blob_id, length; uint64_t data; };

#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0

#define DRM_MODE_DPMS_ON  0
#define DRM_MODE_DPMS_OFF 3

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

struct drm_mode_get_plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };

struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types;
    uint64_t format_type_ptr;
};

/* --- framebuffers and buffers -------------------------------------------- */

struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

#define DRM_MODE_FB_MODIFIERS (1 << 1)

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02

struct drm_clip_rect { uint16_t x1, y1, x2, y2; };

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id, flags, color, num_clips;
    uint64_t clips_ptr;
};

struct drm_mode_cursor {
    uint32_t flags, crtc_id;
    int32_t x, y;
    uint32_t width, height, handle;
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };

struct drm_mode_list_lessees { uint32_t count_lessees, pad; uint64_t lessees_ptr; };

#define DRM_FORMAT_ARGB8888 0x34325241u /* 'AR24' */
#define DRM_FORMAT_XBGR8888 0x34324258u /* 'XB24' */
#define DRM_FORMAT_ABGR8888 0x34324241u /* 'AB24' */

#endif
