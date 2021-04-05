/*
* SPDX-License-Identifier: GPL-2.0
*
* PL MEM Mapping driver
*
* Description:
* This PL MEM driver, allocates coherent memory in either PL or PS
* depending on the selection. Triggers DEV_TO_MEM or MEM_TO_DEVICE
* DMA depending on the usecase.
*
* Copyright (C) 2021 Xilinx, Inc.
*
* Author: Anil Mamidala <amamidal@xilinx.com>
* Co-Author: Nayan Bhavsar <nayan.bhavsar@xilinx.com>
*/

#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <asm/page.h>
#include <asm/byteorder.h>

#define DEVICE_MAX_NUM                          256
#define MAX_INSTANCES                           4
#define DRIVER_NAME                             "pciep"
#define DEVICE_NAME_FORMAT                      "pciep%d"

#define PCIEP_READ_BUFFER_READY                 0x00
#define PCIEP_READ_BUFFER_ADDR                  0x04
#define PCIEP_READ_BUFFER_OFFSET                0x08
#define PCIEP_READ_BUFFER_SIZE                  0x0c
#define PCIEP_WRITE_BUFFER_READY                0x10
#define PCIEP_WRITE_BUFFER_ADDR                 0x14
#define PCIEP_WRITE_BUFFER_OFFSET               0x18
#define PCIEP_WRITE_BUFFER_SIZE                 0x1c
#define PCIEP_READ_TRANSFER_DONE                0x20
#define PCIEP_WRITE_TRANSFER_DONE               0x24
#define PCIEP_READ_TRANSFER_CLR                 0x28
#define PCIEP_READ_BUFFER_HOST_INTR             0x2c
#define PCIEP_WRITE_TRANSFER_CLR                0x30

#define PCIRC_READ_FILE_LENGTH                  0x40
#define PCIRC_READ_BUFFER_TRANSFER_DONE         0x44
#define PCIRC_WRITE_BUFFER_TRANSFER_DONE        0x48
#define PCIRC_ENC_PARAMS_1                      0x4c
#define PCIRC_ENC_PARAMS_2                      0x50
#define PCIRC_RAW_RESOLUTION                    0x54
#define PCIRC_USECASE_MODE                      0x58
#define PCIRC_ENC_PARAMS_3                      0x5c
#define PCIRC_ENC_PARAMS_4                      0x60
#define PCIRC_ENC_PARAMS_5                      0x64
#define PCIRC_READ_BUFFER_TRANSFER_DONE_INTR    0x68
#define PCIRC_WRITE_BUFFER_TRANSFER_DONE_INTR   0x6c
#define PCIRC_HOST_DONE_INTR                    0x70

#define PCIEP_CLR_REG                           0x0
#define CLR_ALL                                 0x0
#define CLR_BUFFER_RDY                          0x0
#define SET_BUFFER_RDY                          0x1
#define SET_TRANSFER_DONE                       0x1

#define GET_FILE_LENGTH                         0x0
#define GET_ENC_PARAMS                          0x1
#define SET_READ_OFFSET                         0x2
#define SET_WRITE_OFFSET                        0x3
#define SET_READ_TRANSFER_DONE                  0x5
#define CLR_READ_TRANSFER_DONE                  0x6
#define SET_WRITE_TRANSFER_DONE                 0x7
#define CLR_WRITE_TRANSFER_DONE                 0x8
#define GET_RESOLUTION                          0x9
#define GET_MODE                                0xa
#define GET_FPS                                 0xb
#define GET_FORMAT                              0xc

#define WIDTH_SHIFT                             0x0
#define WIDTH_MASK                              0xFFFF
#define HEIGHT_SHIFT                            16
#define HEIGHT_MASK                             0xFFFF
#define USE_CASE_MODE_SHIFT                     0x0
#define USE_CASE_MODE_MASK                      0x3
#define FPS_SHIFT                               0x5
#define FPS_MASK                                0x3FF
#define FORMAT_SHIFT                            0x2
#define FORMAT_MASK                             0x7

#define L2CACHE_SHIFT                           0x0
#define L2CACHE_MASK                            0x1
#define LOW_BANDWIDTH_SHIFT                     0x1
#define LOW_BANDWIDTH_MASK                      0x1
#define FILLER_DATA_SHIFT                       0x2
#define FILLER_DATA_MASK                        0x1
#define BITRATE_SHIFT                           0x4
#define BITRATE_MASK                            0xFFFF
#define GOP_LENGTH_SHIFT                        20
#define GOP_LENGTH_MASK                         0x3FF
#define MAX_PICTURE_SIZE_SHIFT                  0x1E
#define MAX_PICTURE_SIZE_MASK                   0x1

#define B_FRAME_SHIFT                           0x0
#define B_FRAME_MASK                            0x3
#define SLICE_SHIFT                             0x3
#define SLICE_MASK                              0x3F
#define QP_MODE_SHIFT                           0x9
#define QP_MODE_MASK                            0x3
#define RC_MODE_SHIFT                           0xb
#define RC_MODE_MASK                            0x3
#define ENC_TYPE_SHIFT                          0xd
#define ENC_TYPE_MASK                           0x3
#define GOP_MODE_SHIFT                          0xf
#define GOP_MODE_MASK                           0x7
#define PROFILE_SHIFT                           0x12
#define PROFILE_MASK                            0x3
#define MIN_QP_SHIFT                            0x14
#define MIN_QP_MASK                             0x3F
#define MAX_QP_SHIFT                            0x1A
#define MAX_QP_MASK                             0x3F
#define CPB_SIZE_SHIFT                          0x0
#define CPB_SIZE_MASK                           0xFFFF
#define INITIAL_DELAY_SHIFT                     0x0
#define INITIAL_DELAY_MASK                      0xFFFF
#define PERIODICITY_IDR_SHIFT                   0x0
#define PERIODICITY_IDR_MASK                    0xFFFF


#define READ_BUF_HIGH_OFFSET                    0xFFFF0000
#define WRITE_BUF_HIGH_OFFSET                   0xFFFF0000

static DEFINE_IDA(pciep_device_ida);
static dev_t  pciep_device_number;
static bool pciep_platform_driver_done;
static struct class *pciep_sys_class;
static DEFINE_MUTEX(pcie_read_mutex);
static DEFINE_MUTEX(pcie_write_mutex);

/**
 * struct pciep_driver_data - Plmem driver data
 * @sys_dev: character device pointer
 * @cdev: character device structure
 * @complete: completion variable
 * @device_number: character driver device number
 * @is_open: holds whether file is opened
 * @size: size of memory pool
 * @count: no.of bytes to transfer
 * @virt_addr: virtual address of memory region
 * @phys_addr: physical address of memory region
 * @mem_used: holds whether pool is uses or not
 */
struct pciep_driver_data {
	struct device *sys_dev;
	struct device *dma_dev;
	void __iomem *regs;
	int rd_irq;
	int wr_irq;
	int host_done_irq;
	struct cdev cdev;
	struct completion read_complete;
	struct completion write_complete;
	dev_t device_number;
	bool is_open;
	int size;
	int count;
	void *read_virt_addr;
	void *write_virt_addr;
	dma_addr_t read_phys_addr;
	dma_addr_t write_phys_addr;
};

typedef struct enc_params {
	bool enable_l2Cache;
	bool low_bandwidth;
	bool filler_data;
	bool max_picture_size;
	unsigned int bitrate;
	unsigned int gop_len;
	unsigned int b_frame;
	unsigned int slice;
	unsigned int qp_mode;
	unsigned int rc_mode;
	unsigned int enc_type;
	unsigned int gop_mode;
	unsigned int profile;
	unsigned int min_qp;
	unsigned int max_qp;
	unsigned int cpb_size;
	unsigned int initial_delay;
	unsigned int periodicity_idr;
} enc_params;

typedef struct resolution {
	unsigned int width;
	unsigned int height;
} resolution;

static inline u32 reg_read(struct pciep_driver_data *this, u32 reg)
{
	return ioread32(this->regs + reg);
}

static inline void reg_write(struct pciep_driver_data *this, u32 reg,
				u32 value)
{
	iowrite32(value, this->regs + reg);
}


static int pcie_reset_all(struct pciep_driver_data *this)
{
	if (this) {
		reg_write(this, PCIEP_READ_TRANSFER_DONE, PCIEP_CLR_REG);
		reg_write(this, PCIEP_WRITE_TRANSFER_DONE, PCIEP_CLR_REG);
		reg_write(this, PCIEP_READ_BUFFER_OFFSET, PCIEP_CLR_REG);
		reg_write(this, PCIEP_READ_BUFFER_SIZE, PCIEP_CLR_REG);
		reg_write(this, PCIEP_WRITE_BUFFER_SIZE, PCIEP_CLR_REG);
		reg_write(this, PCIEP_READ_BUFFER_READY, PCIEP_CLR_REG);
		reg_write(this, PCIEP_WRITE_BUFFER_READY, PCIEP_CLR_REG);
	}
	else {
		return -EINVAL;
	}

	return 0;
}

/**
 * pciep_driver_file_open() - This is the driver open function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * Return:      Success(=0) or error status(<0).
 */
static int pciep_driver_file_open(struct inode *inode, struct file *file)
{
	struct pciep_driver_data *this;
	int status = 0;

	this = container_of(inode->i_cdev, struct pciep_driver_data, cdev);
	file->private_data = this;
	this->is_open = 1;

	pcie_reset_all(this);

	return status;
}

/**
 * pciep_driver_file_release() - This is the driver release function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * Return:      Success(=0) or error status(<0).
 */
static int pciep_driver_file_release(struct inode *inode, struct file *file)
{
	struct pciep_driver_data *this = file->private_data;
	u32 value;

	/* Terminate DMA transfer and reset required flags */
	this->is_open = 0;

	/* clear all the registers */
	reg_write(this, PCIEP_READ_BUFFER_OFFSET, PCIEP_CLR_REG);
	value = reg_read(this, PCIEP_READ_BUFFER_READY);
	value &= ~READ_BUF_HIGH_OFFSET;
	reg_write(this, PCIEP_READ_BUFFER_READY, value);
	reg_write(this, PCIEP_READ_BUFFER_SIZE, PCIEP_CLR_REG);
	reg_write(this, PCIEP_WRITE_BUFFER_SIZE, PCIEP_CLR_REG);

	return 0;
}

/**
 * pciep_driver_file_mmap() - This is the driver memory map function.
 * @file:	Pointer to the file structure.
 * @vma:        Pointer to the vm area structure.
 * Return:      Success(=0) or error status(<0).
 */
static int pciep_driver_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static long pciep_driver_file_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct pciep_driver_data *this = file->private_data;
	unsigned int value;
	u64 value1;
	u64 value2;
	u64 value3;
	u64 value4;
	u64 size;
	struct enc_params params;
	struct resolution res;
	int ret;

	switch (cmd) {
	case GET_FILE_LENGTH:
		value = reg_read(this, PCIRC_READ_FILE_LENGTH);
		value1 = reg_read(this, (PCIRC_READ_FILE_LENGTH - 4));
		size = value | value1 << 32;
		ret = copy_to_user((u64 *) arg, &size, sizeof(size));
		return ret;

	case GET_ENC_PARAMS:
		value = reg_read(this, PCIRC_ENC_PARAMS_1);
		params.enable_l2Cache = (value>>L2CACHE_SHIFT) & L2CACHE_MASK;
		params.low_bandwidth = (value>>LOW_BANDWIDTH_SHIFT) & LOW_BANDWIDTH_MASK;
		params.filler_data = (value>>FILLER_DATA_SHIFT) & FILLER_DATA_MASK;
		params.bitrate = (value>>BITRATE_SHIFT) & BITRATE_MASK;
		params.gop_len = (value>>GOP_LENGTH_SHIFT) & GOP_LENGTH_MASK;
		params.max_picture_size = (value>>MAX_PICTURE_SIZE_SHIFT) & MAX_PICTURE_SIZE_MASK;

		value1 = reg_read(this, PCIRC_ENC_PARAMS_2);
		params.b_frame = (value1>>B_FRAME_SHIFT) & B_FRAME_MASK;
		params.slice = (value1>>SLICE_SHIFT) & SLICE_MASK;
		params.qp_mode = (value1>>QP_MODE_SHIFT) & QP_MODE_MASK;
		params.rc_mode = (value1>>RC_MODE_SHIFT) & RC_MODE_MASK;
		params.enc_type = (value1>>ENC_TYPE_SHIFT) & ENC_TYPE_MASK;
		params.gop_mode = (value1>>GOP_MODE_SHIFT) & GOP_MODE_MASK;
		params.profile = (value1>>PROFILE_SHIFT) & PROFILE_MASK;
		params.min_qp = (value1>>MIN_QP_SHIFT) & MIN_QP_MASK;
		params.max_qp = (value1>>MAX_QP_SHIFT) & MAX_QP_MASK;

		value2 = reg_read(this, PCIRC_ENC_PARAMS_3);
		params.cpb_size = (value2>>CPB_SIZE_SHIFT) & CPB_SIZE_MASK;
		value3 = reg_read(this, PCIRC_ENC_PARAMS_4);
		params.initial_delay = (value3>>INITIAL_DELAY_SHIFT) & INITIAL_DELAY_MASK;
		value4 = reg_read(this, PCIRC_ENC_PARAMS_5);
		params.periodicity_idr = (value4>>PERIODICITY_IDR_SHIFT) & PERIODICITY_IDR_MASK;
		ret = copy_to_user((struct enc_params *) arg, &params, sizeof(params));
		return ret;

	case GET_RESOLUTION:
		value = reg_read(this, PCIRC_RAW_RESOLUTION);
		res.width = (value>>WIDTH_SHIFT) & WIDTH_MASK;
		res.height = (value>>HEIGHT_SHIFT) & HEIGHT_MASK;
		ret = copy_to_user((struct resolution *) arg, &res, sizeof(res));
		return ret;

	case GET_MODE:
		value = reg_read(this, PCIRC_USECASE_MODE);
		value = (value >> USE_CASE_MODE_SHIFT) & USE_CASE_MODE_MASK;
		ret = copy_to_user((u32 *) arg, &value, sizeof(value));
		return ret;

	case SET_READ_OFFSET:
		ret = copy_from_user(&value1, (u64 *) arg, sizeof(value1));
		reg_write(this, PCIEP_READ_BUFFER_OFFSET, value1);
	        value = reg_read(this, PCIEP_READ_BUFFER_READY);
	        value &= ~READ_BUF_HIGH_OFFSET;
	        value |= (value1 >> 16) & READ_BUF_HIGH_OFFSET;
	        reg_write(this, PCIEP_READ_BUFFER_READY, value);
		return ret;

	case SET_WRITE_OFFSET:
		ret = copy_from_user(&value1, (u64 *) arg, sizeof(value1));
		reg_write(this, PCIEP_WRITE_BUFFER_OFFSET, value1);
	        value = reg_read(this, PCIEP_WRITE_BUFFER_READY);
	        value &= ~WRITE_BUF_HIGH_OFFSET;
	        value |= (value1 >> 16) & WRITE_BUF_HIGH_OFFSET;
	        reg_write(this, PCIEP_WRITE_BUFFER_READY, value);
		return ret;

	case SET_READ_TRANSFER_DONE:
		reg_write(this, PCIEP_READ_TRANSFER_DONE, 0xef);
		return 0;

	case SET_WRITE_TRANSFER_DONE:
		reg_write(this, PCIEP_WRITE_TRANSFER_DONE, 0xef);
		return 0;

	case CLR_READ_TRANSFER_DONE:
		reg_write(this, PCIEP_READ_TRANSFER_DONE, 0x00);
		return 0;

	case CLR_WRITE_TRANSFER_DONE:
		reg_write(this, PCIEP_WRITE_TRANSFER_DONE, 0x00);
		return 0;

	case GET_FPS:
		value = reg_read(this, PCIRC_USECASE_MODE);
		value = (value >> FPS_SHIFT) & FPS_MASK;
		ret = copy_to_user((u32 *) arg, &value, sizeof(value));
		return ret;

	case GET_FORMAT:
		value = reg_read(this, PCIRC_USECASE_MODE);
		value = (value >> FORMAT_SHIFT) & FORMAT_MASK;
		ret = copy_to_user((u32 *) arg, &value, sizeof(value));
		return ret;

	default:
		return -ENOTTY;
	}
}

/**
 * pciep_driver_file_read() - This is the driver read function.
 * @file:	Pointer to the file structure.
 * @buff:	Pointer to the user buffer.
 * @count:	The number of bytes to be written.
 * @ppos:	Pointer to the offset value.
 * Return:	Transferred size.
 */
static ssize_t pciep_driver_file_read(struct file *file, char __user *buff,
				      size_t count, loff_t *ppos)
{
	struct pciep_driver_data *this = file->private_data;
	u32 value;
	int ret;

	/* check the size */
	if (count <= 0)
		return -EINVAL;

	/* allocate the buffer based on the size from applcation */
	this->read_virt_addr = dma_alloc_coherent(this->dma_dev, count,
					     &(this->read_phys_addr), GFP_KERNEL);
	if (IS_ERR_OR_NULL(this->read_virt_addr)) {
		dev_err(this->dma_dev, "%s dma_alloc_coherent() failed\n",
			__func__);
		this->read_virt_addr = NULL;
		return -ENOMEM;
	}

	reg_write(this, PCIEP_READ_BUFFER_ADDR, this->read_phys_addr);
	reg_write(this, PCIEP_READ_BUFFER_SIZE, count);
	value = reg_read(this, PCIEP_READ_BUFFER_READY);
	value |= SET_BUFFER_RDY;
	reg_write(this, PCIEP_READ_BUFFER_READY, value);

	/* wait for done event */
	wait_for_completion(&this->read_complete);

	ret = copy_to_user(buff, this->read_virt_addr, count);

	/* free up the allocated memory */
	dma_free_coherent(this->dma_dev, count,
                 this->read_virt_addr, this->read_phys_addr);

	return ret;
}

/**
 * pciep_driver_file_write() - This is the driver write function.
 * @file:	Pointer to the file structure.
 * @buff:	Pointer to the user buffer.
 * @count:	The number of bytes to be written.
 * @ppos:	Pointer to the offset value
 * Return:	Transferred size.
 */
static ssize_t pciep_driver_file_write(struct file *file,
				       const char __user *buff,
				       size_t count, loff_t *ppos)
{
	struct pciep_driver_data *this = file->private_data;
	int ret;
	u32 value;

	/* check the size */
	if (count <= 0)
		return -EINVAL;

	/* dma buffer allocation */
	this->write_virt_addr = dma_alloc_coherent(this->dma_dev, count,
					     &(this->write_phys_addr), GFP_KERNEL);
	if (IS_ERR_OR_NULL(this->write_virt_addr)) {
		dev_err(this->dma_dev, "%s dma_alloc_coherent() failed\n",
			__func__);
		this->write_virt_addr = NULL;
		return -ENOMEM;
	}

	ret = copy_from_user(this->write_virt_addr, buff, count);
	if (ret)
		goto out;

	reg_write(this, PCIEP_WRITE_BUFFER_ADDR, this->write_phys_addr);
	reg_write(this, PCIEP_WRITE_BUFFER_SIZE, count);
	value = reg_read(this, PCIEP_WRITE_BUFFER_READY);
	value |= SET_BUFFER_RDY;
	reg_write(this, PCIEP_WRITE_BUFFER_READY, value);

	/* wait for done event */
	wait_for_completion(&this->write_complete);
out:
	/* free the allocated memory */
	dma_free_coherent(this->dma_dev, count,
                 this->write_virt_addr, this->write_phys_addr);

	return ret;
}


static loff_t pciep_driver_file_lseek(struct file *file,loff_t offset, int orig)
{
	struct pciep_driver_data *this = file->private_data;
	u32 value;

	reg_write(this, PCIEP_READ_BUFFER_OFFSET, offset);
	value = reg_read(this, PCIEP_READ_BUFFER_READY);
	value &= ~READ_BUF_HIGH_OFFSET;
	value |= (offset >> 16) & READ_BUF_HIGH_OFFSET;
	reg_write(this, PCIEP_READ_BUFFER_READY, value);
	return offset;
}

static const struct file_operations pciep_driver_file_ops = {
	.owner   = THIS_MODULE,
	.open    = pciep_driver_file_open,
	.release = pciep_driver_file_release,
	.mmap    = pciep_driver_file_mmap,
	.read    = pciep_driver_file_read,
	.write   = pciep_driver_file_write,
	.llseek  = pciep_driver_file_lseek,
	.unlocked_ioctl = pciep_driver_file_ioctl,
};


/**
 * xilinx_pciep_read_irq_handler - Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the driver data structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_pciep_read_irq_handler(int irq, void *data)
{
	struct pciep_driver_data *driver_data = data;
	u32 value;

	value = reg_read(driver_data, PCIEP_READ_BUFFER_READY);
	value &= ~SET_BUFFER_RDY;
	reg_write(driver_data, PCIEP_READ_BUFFER_READY, value);
	complete(&driver_data->read_complete);
	reg_read(driver_data, PCIRC_READ_BUFFER_TRANSFER_DONE_INTR);

	return IRQ_HANDLED;
}

/**
 * xilinx_pciep_write_irq_handler - Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the driver data structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_pciep_write_irq_handler(int irq, void *data)
{
	u32 value;
	struct pciep_driver_data *driver_data = data;

	value = reg_read(driver_data, PCIEP_WRITE_BUFFER_READY);
	value &= ~SET_BUFFER_RDY;
	reg_write(driver_data, PCIEP_WRITE_BUFFER_READY, value);

	complete(&driver_data->write_complete);
	reg_read(driver_data, PCIRC_WRITE_BUFFER_TRANSFER_DONE_INTR);

	return IRQ_HANDLED;
}

/**
 * xilinx_pciep_write_irq_handler - Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the driver data structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_pciep_host_done_irq_handler(int irq, void *data)
{
	struct pciep_driver_data *driver_data = data;

	reg_read(driver_data, PCIRC_HOST_DONE_INTR);
	reg_write(driver_data, PCIEP_READ_TRANSFER_DONE, PCIEP_CLR_REG);
	reg_write(driver_data, PCIEP_WRITE_TRANSFER_DONE, PCIEP_CLR_REG);

	return IRQ_HANDLED;
}
/**
 * pciep_driver_create() -  Create pciep driver data structure.
 * @name:       device name   or NULL.
 * @parent:     parent device or NULL.
 * @minor:	minor_number.
 * @size:	buffer size.
 * @channel:    DMA channel name
 * Return:      Pointer to the pciep driver data structure or NULL.
 *
 * It does all the memory allocation and registration for the device.
 */
static struct pciep_driver_data *pciep_driver_create(const char *name,
						     struct device *parent,
						     u32 minor, u32 size,
						     char *channel)
{
	struct pciep_driver_data *this = NULL;
	const unsigned int DONE_ALLOC_MINOR   = (1 << 0);
	const unsigned int DONE_CHRDEV_ADD    = (1 << 1);
	const unsigned int DONE_ALLOC_CMA     = (1 << 2);
	const unsigned int DONE_DEVICE_CREATE = (1 << 3);
	unsigned int done = 0;

	/* allocate device minor number */
	if (minor < DEVICE_MAX_NUM) {
		if (ida_simple_get(&pciep_device_ida, minor, minor+1,
				   GFP_KERNEL) < 0) {
			dev_err(parent, "couldn't allocate minor number(=%d)\n",
				minor);
			goto failed;
		}
	} else {
		dev_err(parent, "invalid minor num(=%d),valid range: 0 to %d\n",
			minor, DEVICE_MAX_NUM-1);
		goto failed;
	}
	done |= DONE_ALLOC_MINOR;
	/* create (pciep_driver_data*) this. */
	this = kzalloc(sizeof(*this), GFP_KERNEL);
	if (IS_ERR_OR_NULL(this))
		goto failed;
	/* make this->device_number and this->size */
	this->device_number = MKDEV(MAJOR(pciep_device_number), minor);
	this->size          = size;
	/* register /sys/class/ */
	this->sys_dev = device_create(pciep_sys_class,
			parent,
			this->device_number,
			(void *)this,
			DEVICE_NAME_FORMAT, MINOR(this->device_number));

	if (IS_ERR_OR_NULL(this->sys_dev)) {
		this->sys_dev = NULL;
		goto failed;
	}
	done |= DONE_DEVICE_CREATE;

	/* setup dma_dev */
	this->dma_dev = parent;

	of_dma_configure(this->dma_dev, NULL, true);
	dma_set_mask(this->dma_dev, DMA_BIT_MASK(sizeof(dma_addr_t) * 4));
	dma_set_coherent_mask(this->dma_dev,
			      DMA_BIT_MASK(sizeof(dma_addr_t) * 4));

	done |= DONE_ALLOC_CMA;

	/* add chrdev */
	cdev_init(&this->cdev, &pciep_driver_file_ops);
	this->cdev.owner = THIS_MODULE;
	if (cdev_add(&this->cdev, this->device_number, MAX_INSTANCES) != 0) {
		dev_err(parent, "cdev_add() failed\n");
		goto failed;
	}
	done |= DONE_CHRDEV_ADD;

	dev_info(this->sys_dev, "major number   = %d\n",
		 MAJOR(this->device_number));
	dev_info(this->sys_dev, "minor number   = %d\n",
		MINOR(this->device_number));
	init_completion(&this->read_complete);
	init_completion(&this->write_complete);

	pr_err("pcie end point driver initialization success\n");
	return this;
failed:
	if (done & DONE_CHRDEV_ADD)
		cdev_del(&this->cdev);
	if (done & DONE_DEVICE_CREATE)
		device_destroy(pciep_sys_class, this->device_number);
	if (done & DONE_ALLOC_MINOR)
		ida_simple_remove(&pciep_device_ida, minor);
	if (this != NULL)
		kfree(this);
	return NULL;
}

/**
 * pciep_platform_driver_probe() -  Probe call for the device.
 * @pdev:	handle to the platform device structure.
 * Return:      Success(=0) or error status(<0).
 *
 * It does all the memory allocation and registration for the device.
 */
static int pciep_platform_driver_probe(struct platform_device *pdev)
{
	int retval = 0;
	u32 minor_number = 0;
	struct pciep_driver_data *driver_data;
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	int status;
	int ret;
	u32 size=4096;
	char channel[5];

	/* create (pciep_driver_data*)this. */
	driver_data = pciep_driver_create(DRIVER_NAME, &pdev->dev, minor_number,
					  size, channel);
	if (IS_ERR_OR_NULL(driver_data)) {
		dev_err(&pdev->dev, "driver create fail.\n");
		retval = PTR_ERR(driver_data);
		goto failed;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	driver_data->regs= devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(driver_data->regs))
		return PTR_ERR(driver_data->regs);

	driver_data->rd_irq = irq_of_parse_and_map(node, 0);
	if (driver_data->rd_irq < 0) {
		pr_err("Unable to get IRQ for pcie");
		return driver_data->rd_irq;
	}

	ret = devm_request_irq(&pdev->dev, driver_data->rd_irq,
			       xilinx_pciep_read_irq_handler, IRQF_SHARED,
			       "xilinx_pciep_read", driver_data);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register IRQ\n");
		goto failed;
	}

	driver_data->wr_irq = irq_of_parse_and_map(node, 1);
	if (driver_data->wr_irq < 0) {
		pr_err("Unable to get IRQ1 for pcie");
		return driver_data->wr_irq;
	}

	ret = devm_request_irq(&pdev->dev, driver_data->wr_irq,
			       xilinx_pciep_write_irq_handler, IRQF_SHARED,
			       "xilinx_pciep_write", driver_data);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register IRQ\n");
		goto failed;
	}

	driver_data->host_done_irq = irq_of_parse_and_map(node, 2);
	if (driver_data->host_done_irq < 0) {
		pr_err("Unable to get IRQ1 for pcie");
		return driver_data->host_done_irq;
	}

	ret = devm_request_irq(&pdev->dev, driver_data->host_done_irq,
			       xilinx_pciep_host_done_irq_handler, IRQF_SHARED,
			       "xilinx_host_done", driver_data);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register IRQ\n");
		goto failed;
	}


	dev_set_drvdata(&pdev->dev, driver_data);
	dev_info(&pdev->dev, "pcie driver probe success.\n");
	return 0;

failed:
	dev_info(&pdev->dev, "driver install failed.\n");
	return retval;
}

/**
 * pciep_driver_destroy() -  Remove the pciep driver data structure.
 * @this:       Pointer to the pciep driver data structure.
 * Return:      Success(=0) or error status(<0).
 *
 * Unregister the device after releasing the resources.
 */
static int pciep_driver_destroy(struct pciep_driver_data *this)
{
	if (!this)
		return -ENODEV;

	ida_simple_remove(&pciep_device_ida, MINOR(this->device_number));
	cdev_del(&this->cdev);
	kfree(this);
	return 0;
}

/**
 * pciep_platform_driver_remove() -  Remove call for the device.
 * @pdev:	Handle to the platform device structure.
 * Return:      Success(=0) or error status(<0).
 *
 * Unregister the device after releasing the resources.
 */
static int pciep_platform_driver_remove(struct platform_device *pdev)
{
	struct pciep_driver_data *this = dev_get_drvdata(&pdev->dev);
	int retval = 0;

	retval = pciep_driver_destroy(this);
	if (retval != 0)
		return retval;
	dev_set_drvdata(&pdev->dev, NULL);
	return 0;
}

/**
 * Open Firmware Device Identifier Matching Table
 */
static const struct of_device_id pciep_of_match[] = {
	{ .compatible = "xlnx,pcie-reg-space-v1-0-1.0", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, pciep_of_match);

/**
 * Platform Driver Structure
 */
static struct platform_driver pciep_platform_driver = {
	.probe  = pciep_platform_driver_probe,
	.remove = pciep_platform_driver_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name  = DRIVER_NAME,
		.of_match_table = pciep_of_match,
	},
};


/**
 * pciep_module_exit()
 */
static void pciep_module_exit(void)
{

	if (pciep_platform_driver_done)
		platform_driver_unregister(&pciep_platform_driver);
	if (pciep_device_number != 0)
		unregister_chrdev_region(pciep_device_number, 0);
	ida_destroy(&pciep_device_ida);
}

/**
 * pciep_module_init()
 */
static int __init pciep_module_init(void)
{
	int retval = 0;

	ida_init(&pciep_device_ida);
	retval = alloc_chrdev_region(&pciep_device_number, 0, MAX_INSTANCES,
				     DRIVER_NAME);
	if (retval != 0) {
		pr_err("%s: couldn't allocate device major number\n",
		       DRIVER_NAME);
		pciep_device_number = 0;
		goto failed;
	}
	pciep_sys_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR_OR_NULL(pciep_sys_class)) {
		pr_err("%s: couldn't create sys class\n", DRIVER_NAME);
		retval = PTR_ERR(pciep_sys_class);
		pciep_sys_class = NULL;
		goto failed;
	}
	retval = platform_driver_register(&pciep_platform_driver);
	if (retval)
		pr_err("%s: couldn't register platform driver\n", DRIVER_NAME);
	else
		pciep_platform_driver_done = 1;
	return 0;
failed:
	pciep_module_exit();
	return retval;
}

module_init(pciep_module_init);
module_exit(pciep_module_exit);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("PCIe usersapce register device driver");
MODULE_LICENSE("GPL v2");
