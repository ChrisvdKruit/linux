#ifndef TARGET_CORE_IBLOCK_H
#define TARGET_CORE_IBLOCK_H

#define IBLOCK_VERSION		"4.0"

#define IBLOCK_HBA_QUEUE_DEPTH	512
#define IBLOCK_DEVICE_QUEUE_DEPTH	32
#define IBLOCK_MAX_DEVICE_QUEUE_DEPTH	128
#define IBLOCK_MAX_CDBS		16
#define IBLOCK_LBA_SHIFT	9

extern struct se_global *se_global;

struct iblock_req {
	unsigned char ib_scsi_cdb[MAX_COMMAND_SIZE];
	atomic_t ib_bio_cnt;
	u32	ib_sg_count;
	void	*ib_buf;
	struct bio *ib_bio;
	struct iblock_dev *ib_dev;
} ____cacheline_aligned;

#define IBDF_HAS_UDEV_PATH		0x01
#define IBDF_HAS_FORCE			0x02
#define IBDF_BDEV_EXCLUSIVE		0x04
#define IBDF_BDEV_ISSUE_FLUSH		0x08

struct iblock_dev {
	unsigned char ibd_udev_path[SE_UDEV_PATH_LEN];
	int	ibd_force;
	int	ibd_major;
	int	ibd_minor;
	u32	ibd_depth;
	u32	ibd_flags;
	struct bio_set	*ibd_bio_set;
	struct block_device *ibd_bd;
	struct iblock_hba *ibd_host;
} ____cacheline_aligned;

struct iblock_hba {
	int		iblock_host_id;
} ____cacheline_aligned;

#endif /* TARGET_CORE_IBLOCK_H */
