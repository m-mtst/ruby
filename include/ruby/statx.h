struct statx_timestamp
{
  __int64_t tv_sec;
  __int64_t tv_nsec;
  __int32_t __statx_timestamp_pad1[1];
};

/* Warning: The kernel may add additional fields to this struct in the
   future.  Only use this struct for calling the statx function, not
   for storing data.  (Expansion will be controlled by the mask
   argument of the statx function.)  */
struct statx
{
  __uint32_t stx_mask;
  __uint32_t stx_blksize;
  __uint64_t stx_attributes;
  __uint32_t stx_nlink;
  __uint32_t stx_uid;
  __uint32_t stx_gid;
  __uint16_t stx_mode;
  __uint16_t __statx_pad1[1];
  __uint64_t stx_ino;
  __uint64_t stx_size;
  __uint64_t stx_blocks;
  __uint64_t stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  __uint32_t stx_rdev_major;
  __uint32_t stx_rdev_minor;
  __uint32_t stx_dev_major;
  __uint32_t stx_dev_minor;
  __uint64_t __statx_pad2[14];
};

#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_ALL 0x0fffU
#define STATX_BTIME 0x0800U
#define STATX__RESERVED 0x80000000U

#define STATX_ATTR_COMPRESSED 0x0004
#define STATX_ATTR_IMMUTABLE 0x0010
#define STATX_ATTR_APPEND 0x0020
#define STATX_ATTR_NODUMP 0x0040
#define STATX_ATTR_ENCRYPTED 0x0800
#define STATX_ATTR_AUTOMOUNT 0x1000

#define AT_STATX_SYNC_AS_STAT	0x0000

#define AT_EMPTY_PATH		0x1000
#define AT_NO_AUTOMOUNT	0x800
