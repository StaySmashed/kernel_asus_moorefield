/*
 * flashnode.c	FLASHNODE flash LED driver
 *
 * Copyright 2014 Asus Corporation.
 * Author : Chung-Yi Chou <chung-yi_chou@asus.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <media/flashnode.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/atomisp.h>
#include <linux/proc_fs.h>
#include <asm/intel_scu_pmic.h>
#include <linux/HWVersion.h>
#include <asm/intel-mid.h>
#include <linux/workqueue.h>

//PMIC GPIO4
#define GPIO4CTLO_REG			0x82
#define GPIO4CTLI_REG			0x92


/* Registers */
#define FLASHNODE_FLASH1_CURRENT		0x00
#define FLASHNODE_FLASH2_CURRENT		0x01
#define FLASHNODE_FLASH_TIMER		0x02
#define FLASHNODE_MOVIE_MODE_CURRENT	0x03
#define FLASHNODE_CONTROL1		0x04
#define FLASHNODE_CONTROL2		0x05
#define FLASHNODE_CONTROL3		0x06
#define FLASHNODE_BLINKING_MODE1_TIME	0x07
#define FLASHNODE_BLINKING_MODE2_TIME	0x08
#define FLASHNODE_FAULT			0x09
#define FLASHNODE_INPUT_MONITOR1		0x0A
#define FLASHNODE_INPUT_MONITOR2		0x0B

/* bit mask */
#define FLASHNODE_IFL1			0x1F
#define FLASHNODE_IFL2			0x1F
#define FLASHNODE_FLTM2			0xF0
#define FLASHNODE_FLTM1			0x0F
#define FLASHNODE_IMM2			0xF0
#define FLASHNODE_IMM1			0x0F
#define FLASHNODE_BLK_EN2		0x40
#define FLASHNODE_FL_EN2			0x20
#define FLASHNODE_MM_EN2			0x10
#define FLASHNODE_BLK_EN1		0x04
#define FLASHNODE_FL_EN1			0x02
#define FLASHNODE_MM_EN1			0x01
#define FLASHNODE_RESET			0x80
#define FLASHNODE_FLINHM			0x10
#define FLASHNODE_FLT_INH		0x08
#define FLASHNODE_ILIM			0x06
#define FLASHNODE_VINM			0x01
#define FLASHNODE_VINHYS			0xF0
#define FLASHNODE_VINTH			0x0F
#define FLASHNODE_TOFF1			0xF0
#define FLASHNODE_TON1			0x0F
#define FLASHNODE_TOFF2			0xF0
#define FLASHNODE_TON2			0x0F
#define FLASHNODE_VINMONEX		0x80
#define FLASHNODE_SC			0x40
#define FLASHNODE_OC			0x20
#define FLASHNODE_OTMP			0x10
#define FLASHNODE_FLED2			0x0C
#define FLASHNODE_FLED1			0x03
#define FLASHNODE_IFL_MON1		0x1F
#define FLASHNODE_IFL_MON2		0x1F

#define CTZ(b) __builtin_ctz(b)

#define FLASHNODE_NAME			"flashnode"
#define FLASHNODE_FLASH_MAX_BRIGHTNESS	FLASHNODE_FLASH_CURRENT_750MA
#define FLASHNODE_MOVIE_MAX_BRIGHTNESS	10
#define FLASHNODE_BLINK_MAX_BRIGHTNESS	FLASHNODE_MOVIE_MAX_BRIGHTNESS
#define FLASHNODE_FLASH_MAX_TIMER	FLASHNODE_FLASHTIMEOUT_1425MS
#define FLASHNODE_FLASH_DEFAULT_TIMER  FLASHNODE_FLASHTIMEOUT_1425MS

#define flashnode_suspend NULL
#define flashnode_resume  NULL


enum flashnode_torch_current{
	FLASHNODE_TORCH_CURRENT_25MA, //0
	FLASHNODE_TORCH_CURRENT_50MA,
	FLASHNODE_TORCH_CURRENT_75MA,
	FLASHNODE_TORCH_CURRENT_100MA,
	FLASHNODE_TORCH_CURRENT_125MA,
	FLASHNODE_TORCH_CURRENT_150MA,
	FLASHNODE_TORCH_CURRENT_175MA,
	FLASHNODE_TORCH_CURRENT_200MA,
	FLASHNODE_TORCH_CURRENT_225MA,
	FLASHNODE_TORCH_CURRENT_250MA, // 9
	FLASHNODE_TORCH_CURRENT_NUM,
};


int inline flashnode_mapping_torch_intensity_driver(int light_intensity_percentage){
    int ret;
    ret = FLASHNODE_TORCH_CURRENT_25MA;
    if(light_intensity_percentage > 10)
        ret = FLASHNODE_TORCH_CURRENT_25MA;
    if(light_intensity_percentage > 20)
        ret = FLASHNODE_TORCH_CURRENT_50MA;
    if(light_intensity_percentage > 30)
        ret = FLASHNODE_TORCH_CURRENT_75MA;
    if(light_intensity_percentage > 40)
        ret = FLASHNODE_TORCH_CURRENT_100MA;
    if(light_intensity_percentage > 60)
        ret = FLASHNODE_TORCH_CURRENT_125MA;
    if(light_intensity_percentage > 80)
    	ret = FLASHNODE_TORCH_CURRENT_150MA;
	if(light_intensity_percentage > 90)
    	ret = FLASHNODE_TORCH_CURRENT_175MA;
    return ret;
}


struct flashnode {
	struct v4l2_subdev sd;
	struct mutex power_lock;
	int power_count;
	struct regmap *map;
	int enable_by_pin;
	unsigned char fault;
	enum flashnode_flash_current flash_current;
	enum flashnode_torch_current torch_current;
	enum flashnode_flash_timeout timeout;
	enum flashnode_flash_mode mode;
	struct flashnode_platform_data *pdata;
 	int irq;
	int gpio;
};
#define to_flashnode(p_sd)	container_of(p_sd, struct flashnode, sd)

static int light_record;
static struct flashnode *inner_flashnode;
#ifdef ENABLE_FLASHNODE_V4L2_CONTROL
static int low_disable;
static int high_disable;
static int low_torch_disable;
static int high_torch_disable;
#endif
static struct workqueue_struct *flt_wq;
static struct delayed_work flt_sensor_dowork;

static int camera_open;

struct flashnode_ctrl_id {
	struct v4l2_queryctrl qc;
	int (*s_ctrl) (struct v4l2_subdev *sd, __u32 val);
	int (*g_ctrl) (struct v4l2_subdev *sd, __s32 *val);
};

#ifdef ENABLE_FLASHNODE_V4L2_CONTROL
static int flashnode_set_mode(struct flashnode *flash, unsigned int new_mode)
{
	int ret;
	int value;

	switch (new_mode) {
		case FLASHNODE_MODE_SHUTDOWN:
			printk("[AsusFlash] flash shutdown\n");
			ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x0);
			break;
		case FLASHNODE_MODE_FLASH:
			printk("[AsusFlash] flash on\n");
			if( high_disable && low_disable ){
				printk("[AsusFlash] disable dual flash\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x00);
				ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT, &value );
				printk("[AsusFlash] high flash set intensity (%x)\n",value);
				ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT, &value );
				printk("[AsusFlash] low flash set intensity (%x)\n",value);
			}else if( low_disable ){
				printk("[AsusFlash] disable low flash\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x02);
				ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT, &value );
				printk("[AsusFlash] high flash set intensity (%x)\n",value);
				ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT, &value );
				printk("[AsusFlash] low flash set intensity (%x)\n",value);
			}else if( high_disable ){
				printk("[AsusFlash] disable high flash\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x20);
				ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT, &value );
				printk("[AsusFlash] high flash set intensity (%x)\n",value);
				ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT, &value );
				printk("[AsusFlash] low flash set intensity (%x)\n",value);
			}else{
				printk("[AsusFlash] dual flash on\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x22);
				ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT, &value );
				printk("[AsusFlash] high flash set intensity (%x)\n",value);
				ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT, &value );
				printk("[AsusFlash] low flash set intensity (%x)\n",value);
			}
			break;
		case FLASHNODE_MODE_INDICATOR:
			printk("[AsusFlash] TORCH_INDICATOR on\n");
			if( high_torch_disable  && low_torch_disable ){
				printk("[AsusFlash] disable dual flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x00);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else if( low_torch_disable ){
				printk("[AsusFlash] disable low flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x01);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else if( high_torch_disable ){
				printk("[AsusFlash] disable high flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x10);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else{
				printk("[AsusFlash] dual flash torch on\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x11);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}
			break;
		case FLASHNODE_MODE_TORCH:
			printk("[AsusFlash] TORCH on\n");
			if( high_torch_disable  && low_torch_disable ){
				printk("[AsusFlash] disable dual flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x00);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else if( low_torch_disable ){
				printk("[AsusFlash] disable low flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x01);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else if( high_torch_disable ){
				printk("[AsusFlash] disable high flash torch\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x10);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}else{
				printk("[AsusFlash] dual flash torch on\n");
				ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x11);
				ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
				printk("[AsusFlash] torch intensity (%x)\n",value);
			}
			break;
		default:
			return -EINVAL;
	}
	if (ret == 0)
		flash->mode = new_mode;
	return ret;
}

static int flashnode_set_flash(struct flashnode *flash)
{
	int val_low,val_high,value;
	int ret;

	val_low = flash->flash_current >> 8;       //Low temp
	val_high = (flash->flash_current ) &  0xff; //High temp

	printk("[AsusFlash] flash intensity = %d (%d,%d) \n", flash->flash_current , val_low , val_high  );

	if(val_low == 0){
		low_disable = true;
	}else{
		low_disable = false;
		val_low = val_low - 1;
	}

	if(val_high == 0){
		high_disable = true;
	}else{
		high_disable = false;
		val_high = val_high - 1;
	}

	if(val_low > FLASHNODE_FLASH_MAX_BRIGHTNESS || val_low < 0 ){
		val_low = FLASHNODE_FLASH_MAX_BRIGHTNESS;
	}

	if(val_high > FLASHNODE_FLASH_MAX_BRIGHTNESS || val_high < 0 ){
		val_high = FLASHNODE_FLASH_MAX_BRIGHTNESS;
	}

	ret = regmap_write(flash->map,FLASHNODE_FLASH1_CURRENT,val_high);
	ret = regmap_write(flash->map,FLASHNODE_FLASH2_CURRENT,val_low);

	ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT, &value );
	printk("[AsusFlash] high flash set intensity (%x)\n",value);
	ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT, &value );
	printk("[AsusFlash] low flash set intensity (%x)\n",value);

	flash->mode = FLASHNODE_MODE_FLASH;
	return ret;
}

static int flashnode_set_torch(struct flashnode *flash)
{
	int val_low,val_high,value;
	int ret;

	val_low = flash->torch_current >> 8;       //Low temp
	val_high = (flash->torch_current ) &  0xff; //High temp

	printk("[AsusFlash] torch intensity = %d (%d,%d)\n", flash->torch_current , val_low , val_high);

	if(val_low == 0){
		low_torch_disable = true;
	}else{
		low_torch_disable = false;
		val_low = val_low - 1;
	}

	if(val_high == 0){
		high_torch_disable = true;
	}else{
		high_torch_disable = false;
		val_high = val_high - 1;
	}
	}

	ret = regmap_write(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, (val_low << 4 | val_high) );
	ret = regmap_read(flash->map,FLASHNODE_MOVIE_MODE_CURRENT, &value );
	printk("[AsusFlash] torch set intensity (%x)\n",value);
	flash->mode = FLASHNODE_MODE_TORCH;

	return ret;
}
#endif

static const struct regmap_config flashnode_config =
{
	.reg_bits = 8,
	.val_bits = 8,
};


/* -----------------------------------------------------------------------------
 * V4L2 controls
 */
#ifdef ENABLE_FLASHNODE_V4L2_CONTROL
static int flashnode_s_flash_timeout(struct v4l2_subdev *sd, u32 val)
{
	struct flashnode *flash = to_flashnode(sd);
	int ret;
// Protect: Setting this to 1425ms
	if( val > FLASHNODE_FLASH_MAX_TIMER || val < FLASHNODE_FLASHTIMEOUT_OFF){
		val = FLASHNODE_FLASH_MAX_TIMER;
	}
	ret = regmap_write(flash->map,FLASHNODE_FLASH_TIMER, (val << 4 | val) );
	flash->timeout = val;
	return 0;
}

static int flashnode_g_flash_timeout(struct v4l2_subdev *sd, s32 *val)
{
	struct flashnode *flash = to_flashnode(sd);
	*val = flash->timeout;
	return 0;
}

static int flashnode_s_flash_intensity(struct v4l2_subdev *sd, u32 intensity)
{

	struct flashnode *flash = to_flashnode(sd);

	flash->flash_current = intensity;

	return flashnode_set_flash(flash);
}

static int flashnode_g_flash_intensity(struct v4l2_subdev *sd, s32 *val)
{

	struct flashnode *flash = to_flashnode(sd);
	int value1,value2;
	int ret;
	ret = regmap_read(flash->map,FLASHNODE_FLASH1_CURRENT,&value1);
	ret = regmap_read(flash->map,FLASHNODE_FLASH2_CURRENT,&value2);
	*val = value1 * 100 + value2;

	return 0;
}

static int flashnode_s_torch_intensity(struct v4l2_subdev *sd, u32 intensity)
{

	struct flashnode *flash = to_flashnode(sd);
/*
	Todo : Mapping minimum and maximum current for torch mode.
*/
	flash->torch_current = intensity;

	return flashnode_set_torch(flash);
}

static int flashnode_g_torch_intensity(struct v4l2_subdev *sd, s32 *val)
{

	struct flashnode *flash = to_flashnode(sd);
	*val = flash->torch_current;
	return 0;
}

static int flashnode_s_indicator_intensity(struct v4l2_subdev *sd, u32 intensity)
{

	struct flashnode *flash = to_flashnode(sd);
/*
	Todo : Mapping minimum and maximum current for torch mode.
*/
	flash->torch_current = intensity;

	return flashnode_set_torch(flash);
}

static int flashnode_g_indicator_intensity(struct v4l2_subdev *sd, s32 *val)
{
	struct flashnode *flash = to_flashnode(sd);

	*val = (u32)flash->torch_current;

	return 0;
}

static int flashnode_s_flash_strobe(struct v4l2_subdev *sd, u32 val)
{
	struct flashnode *flash = to_flashnode(sd);
	int ret;
	ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x44);
	return ret;
}

static int flashnode_s_flash_mode(struct v4l2_subdev *sd, u32 new_mode)
{
	struct flashnode *flash = to_flashnode(sd);
	unsigned int mode;
	switch (new_mode) {
	case ATOMISP_FLASH_MODE_OFF:
		mode = FLASHNODE_MODE_SHUTDOWN;
		break;
	case ATOMISP_FLASH_MODE_FLASH:
		mode = FLASHNODE_MODE_FLASH;
		break;
	case ATOMISP_FLASH_MODE_INDICATOR:
		mode = FLASHNODE_MODE_INDICATOR;
		break;
	case ATOMISP_FLASH_MODE_TORCH:
		mode = FLASHNODE_MODE_TORCH;
		break;
	default:
		return -EINVAL;
	}

	return flashnode_set_mode(flash, mode);
}

static int flashnode_g_flash_mode(struct v4l2_subdev *sd, s32 * val)
{
	struct flashnode *flash = to_flashnode(sd);
	*val = flash->mode;
	return 0;
}

static int flashnode_g_flash_status(struct v4l2_subdev *sd, s32 *val)
{
	struct flashnode *flash = to_flashnode(sd);
	int ret;

	ret = regmap_read(flash->map, FLASHNODE_CONTROL1,val);
	if (ret < 0)
		return ret;
/*
	It should be mapped to enum atomisp_flash_status
	for
	ATOMISP_FLASH_STATUS_OK,
	ATOMISP_FLASH_STATUS_HW_ERROR,
	ATOMISP_FLASH_STATUS_INTERRUPTED,
	ATOMISP_FLASH_STATUS_TIMEOUT,
*/
	return 0;
}

static int flashnode_g_flash_status_register(struct v4l2_subdev *sd, s32 *val)
{

	struct flashnode *flash = to_flashnode(sd);
	int ret;

	ret = regmap_read(flash->map, FLASHNODE_CONTROL1,val);

	if (ret < 0)
		return ret;

	return 0;
}
#endif

static const struct flashnode_ctrl_id flashnode_ctrls[] = {
#ifdef ENABLE_FLASHNODE_V4L2_CONTROL
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_TIMEOUT,
				"Flash Timeout",
				0,
				1024,
				1,
				1024,
				0,
				flashnode_s_flash_timeout,
				flashnode_g_flash_timeout),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_INTENSITY,
				"Flash Intensity",
				0,
				255,
				1,
				255,
				0,
				flashnode_s_flash_intensity,
				flashnode_g_flash_intensity),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_TORCH_INTENSITY,
				"Torch Intensity",
				0,
				255,
				1,
				100,
				0,
				flashnode_s_torch_intensity,
				flashnode_g_torch_intensity),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_INDICATOR_INTENSITY,
				"Indicator Intensity",
				0,
				255,
				1,
				100,
				0,
				flashnode_s_indicator_intensity,
				flashnode_g_indicator_intensity),
	s_ctrl_id_entry_boolean(V4L2_CID_FLASH_STROBE,
				"Flash Strobe",
				0,
				0,
				flashnode_s_flash_strobe,
				NULL),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_MODE,
				"Flash Mode",
				0,   /* don't assume any enum ID is first */
				100, /* enum value, may get extended */
				1,
				ATOMISP_FLASH_MODE_OFF,
				0,
				flashnode_s_flash_mode,
				flashnode_g_flash_mode),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_STATUS,
				"Flash Status",
				0,   /* don't assume any enum ID is first */
				100, /* enum value, may get extended */
				1,
				ATOMISP_FLASH_STATUS_OK,
				0,
				NULL,
				flashnode_g_flash_status),
	s_ctrl_id_entry_integer(V4L2_CID_FLASH_STATUS_REGISTER,
				"Flash Status Register",
				0,   /* don't assume any enum ID is first */
				100, /* enum value, may get extended */
				1,
				0,
				0,
				NULL,
				flashnode_g_flash_status_register),
#endif
};

static const struct flashnode_ctrl_id *find_ctrl_id(unsigned int id)
{
	int i;
	int num;

	num = ARRAY_SIZE(flashnode_ctrls);
	for (i = 0; i < num; i++) {
		if (flashnode_ctrls[i].qc.id == id)
			return &flashnode_ctrls[i];
	}

	return NULL;
}

static int flashnode_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	int num;

	if (!qc)
		return -EINVAL;

	num = ARRAY_SIZE(flashnode_ctrls);
	if (qc->id >= num)
		return -EINVAL;

	*qc = flashnode_ctrls[qc->id].qc;

	return 0;
}

static int flashnode_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	const struct flashnode_ctrl_id *s_ctrl;

	if (!ctrl)
		return -EINVAL;

	s_ctrl = find_ctrl_id(ctrl->id);
	if (!s_ctrl)
		return -EINVAL;

	return s_ctrl->s_ctrl(sd, ctrl->value);
}

static int flashnode_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	const struct flashnode_ctrl_id *s_ctrl;

	if (!ctrl)
		return -EINVAL;

	s_ctrl = find_ctrl_id(ctrl->id);
	if (s_ctrl == NULL)
		return -EINVAL;

	return s_ctrl->g_ctrl(sd, &ctrl->value);
}

static int flashnode_setup(struct flashnode *flash)
{
	struct i2c_client *client = v4l2_get_subdevdata(&flash->sd);
	struct regmap *map;
	unsigned char value;
	int ret = 0;

	map = devm_regmap_init_i2c(client, &flashnode_config);
	if (IS_ERR(map)){
		return PTR_ERR(map);
	}



	switch(flash->pdata->current_limit) {
		case 2420: /* 2420mA to 3080mA */
			value = 0 << CTZ(FLASHNODE_ILIM);
			break;
		case 2600: /* 2600mA to 3300mA */
			value = 1 << CTZ(FLASHNODE_ILIM);
			break;
		case 2800: /* 2800mA to 3600mA */
			value = 2 << CTZ(FLASHNODE_ILIM);
			break;
		case 3000: /* 3000mA to 3950mA */
			value = 3 << CTZ(FLASHNODE_ILIM);
			break;
		default:
			return -EINVAL;
	}

	if (flash->pdata->disable_short_led_report)
		value |= FLASHNODE_FLT_INH;

	if (flash->pdata->shutoff_on_inhibit_mode)
		value |= FLASHNODE_FLINHM;

	if (flash->pdata->enable_voltage_monitor)
		value |= FLASHNODE_VINM;

	ret = regmap_write(map, FLASHNODE_CONTROL2, value);
	if (IS_ERR_VALUE(ret))
		return ret;


	if (!flash->pdata->enable_voltage_monitor){
		flash->map = map;
		return 0;
	}

	if (flash->pdata->input_voltage_threshold < 2800 ||
			flash->pdata->input_voltage_threshold > 3900)
		return -EINVAL;

	value = ((flash->pdata->input_voltage_threshold - 2800) / 100)
			<< CTZ(FLASHNODE_VINTH);
	if (flash->pdata->input_voltage_hysteresis < 2900 ||
			flash->pdata->input_voltage_hysteresis > 4000)
		return -EINVAL;

	value |= (((flash->pdata->input_voltage_hysteresis - 2900) / 100) + 1 )
			<< CTZ(FLASHNODE_VINHYS);

	ret = regmap_write(map, FLASHNODE_CONTROL3, value);


	flash->timeout = FLASHNODE_FLASH_DEFAULT_TIMER;
	ret = regmap_write(map, FLASHNODE_FLASH_TIMER , (flash->timeout << 4 | flash->timeout) );

	flash->map = map;

	return ret;
}

static int __flashnode_s_power(struct flashnode *flash, int power)
{
	return 0;
}

static int flashnode_s_power(struct v4l2_subdev *sd, int power)
{
	struct flashnode *flash = to_flashnode(sd);
	int ret = 0;

	mutex_lock(&flash->power_lock);

//Due to the same i2c address issue, we should let i2c switch to SOC/External ISP for control.
//Using GPIO 52 to control this i2c switch. LOW for SOC control;HIGH for External ISP control.
	if(power == 1){
		ret = gpio_request(52, "flash_enable");
		gpio_direction_output(52, 0);
		printk("Lower the GPIO52 when flash node on\n");
	}else{
		ret = gpio_request(52, "flash_enable");
		gpio_direction_output(52, 1);
		printk("Higher the GPIO52 when flash node off\n");
	}

	if (flash->power_count == !power) {
		ret = __flashnode_s_power(flash, !!power);
		if (ret < 0)
			goto done;
	}

	flash->power_count += power ? 1 : -1;
	WARN_ON(flash->power_count < 0);

done:
	mutex_unlock(&flash->power_lock);
	return ret;
}

static void flashnode_torch_on_num(struct flashnode *flash , int intensity , int num){
	int ret;
	if( intensity < FLASHNODE_TORCH_CURRENT_25MA || intensity > FLASHNODE_TORCH_CURRENT_250MA ){
		intensity = FLASHNODE_TORCH_CURRENT_250MA;
	}
	printk("[AsusFlash] Set Torch on %u \n", ( intensity << 4 | intensity ));
	ret = regmap_write(flash->map,FLASHNODE_MOVIE_MODE_CURRENT,( intensity << 4 | intensity ) );

	if( num == 0){
		ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x11);
	}else if( num == 1){
		//For Hight temperature LED (PR)
		ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x01);
	}else if( num == 2){
		//For Low temperature LED   (PR)
		ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x10);
	}
}

static void flashnode_torch_on(struct flashnode *flash , int intensity){
	int ret;
	if( intensity < FLASHNODE_TORCH_CURRENT_25MA || intensity > FLASHNODE_TORCH_CURRENT_250MA ){
		intensity = FLASHNODE_TORCH_CURRENT_250MA;
	}
	printk("[AsusFlash] Set Torch on %u \n", ( intensity << 4 | intensity ));
	ret = regmap_write(flash->map,FLASHNODE_MOVIE_MODE_CURRENT,( intensity << 4 | intensity ) );
	ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x11);
}

static void flashnode_flash_on(struct flashnode *flash){
	int ret;
	printk("[AsusFlash] Set Flash on \n");
	ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x22);
}

static void flashnode_flash_off(struct flashnode *flash){
	int ret;
	printk("[AsusFlash] Set Flash off \n");
	ret = regmap_write(flash->map,FLASHNODE_CONTROL1,0x0);
}

static long flashnode_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{

	struct flashnode *flash = to_flashnode(sd);
	int input_arg = 0;

	switch (cmd) {
	case ATOMISP_TEST_CMD_SET_TORCH:
		input_arg = *(int *)arg;
		if(input_arg == 0){
			flashnode_flash_off(flash);
		}else{
			flashnode_torch_on(flash , 4);
		}
		return 0;
	case ATOMISP_TEST_CMD_SET_FLASH:
		input_arg = *(int *)arg;
		if(input_arg == 0){
			flashnode_flash_off(flash);
		}else{
			flashnode_flash_on(flash);
		}
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}

int set_torch_on_via_SOC(bool enable){
	int ret = 0;
	printk("[TorchControl] Lower the GPIO52 when control flash node on (%d)\n",enable);

	if(inner_flashnode==NULL) return -EINVAL;

	ret = gpio_request(52, "flash_enable");
	gpio_direction_output(52, 0);
	if(enable){
		flashnode_torch_on_num(inner_flashnode,100,1);
	}else{
		flashnode_flash_off(inner_flashnode);
	}
	ret = gpio_request(52, "flash_enable");
	gpio_direction_output(52, 1);
	printk("[TorchControl] Higher the GPIO52 when control flash node off\n");
	return ret;
}
EXPORT_SYMBOL(set_torch_on_via_SOC);

static ssize_t camera_show(struct file *dev, char *buffer, size_t count, loff_t *ppos)
{
	char *buff;
	ssize_t ret = 0;
	int len = 0;
	int status;

	status = Get_Camera_Status();
	printk(KERN_INFO "[Camera_Status] get = %d/%d\n", camera_open,status);
	buff = kmalloc(100,GFP_KERNEL);
	if(!buff)
		return -ENOMEM;

	len += sprintf(buff+len, "%d\n", status);
	ret = simple_read_from_buffer(buffer,count,ppos,buff,len);
	kfree(buff);
	return ret;
}

static ssize_t camera_store(struct file *dev, const char *buf, size_t count, loff_t *loff)
{
	int camera_status = -1;

	sscanf(buf, "%d ", &camera_status);
	if(camera_status > 0){
		camera_open = 1;
	}else{
		camera_open = 0;
	}
	printk(KERN_INFO "[Camera_Status] get = %d , set = %d\n", camera_status, camera_open);
	Set_Camera_Status(camera_open);
	return count;
}

static ssize_t flashnode_show(struct file *dev, char *buffer, size_t count, loff_t *ppos)
{

	int len = 0;
	ssize_t ret = 0;
	char *buff;

	printk(KERN_INFO "[AsusFlash] Read Flash %d\n", light_record);
	buff = kmalloc(100,GFP_KERNEL);
	if(!buff)
		return -ENOMEM;

	len += sprintf(buff+len, "%d\n", light_record);
	ret = simple_read_from_buffer(buffer,count,ppos,buff,len);
	kfree(buff);

	return ret;

}

static ssize_t flashnode_store(struct file *dev, const char *buf, size_t count, loff_t *loff)
{
	int set_light = -1;
	// int map_offset = FLASHNODE_TORCH_CURRENT_NUM;
	int map_num;
	int ret;

	sscanf(buf, "%d", &set_light);
	printk(KERN_INFO "[AsusFlash] Set light to %d\n", set_light);
	if ( (set_light == light_record)){
		return count;
	}

	if(inner_flashnode==NULL) return 0;

	if(set_light < 0 || set_light >200){
		return -1;
	}else if (set_light == 0 ){
		flashnode_flash_off(inner_flashnode);
		light_record = set_light;
		ret = gpio_request(52, "flash_enable");
		gpio_direction_output(52, 1);
		printk("Higher the GPIO52 when flash node off\n");
	}else{
		printk("Lower the GPIO52 when flash node on\n");
		ret = gpio_request(52, "flash_enable");
		gpio_direction_output(52, 0);
		light_record = set_light;
		flashnode_flash_off(inner_flashnode);
		// map_num = set_light - (set_light % map_offset);
		// map_num = map_num / (2 * map_offset);
		map_num = flashnode_mapping_torch_intensity_driver(set_light);
		printk(KERN_INFO "[AsusFlash] Real set light to %d\n", map_num);
		//set 2 for high tempature LED.
		flashnode_torch_on_num(inner_flashnode,map_num,1);
	}

	return count;
}

static const struct file_operations flash_proc_fops = {
	.read = flashnode_show,
	.write = flashnode_store,
};

static const struct file_operations camera_proc_fops = {
	.read = camera_show,
	.write = camera_store,
};

static const struct v4l2_subdev_core_ops flashnode_core_ops = {
	.queryctrl = flashnode_queryctrl,
	.g_ctrl = flashnode_g_ctrl,
	.s_ctrl = flashnode_s_ctrl,
	.s_power = flashnode_s_power,
	.ioctl = flashnode_ioctl,
};

static const struct v4l2_subdev_ops flashnode_ops = {
	.core = &flashnode_core_ops,
};


static int flashnode_detect(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_adapter *adapter = client->adapter;
	struct flashnode *flash = to_flashnode(sd);
	int ret;
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "flashnode_detect i2c error\n");
		return -ENODEV;
	}

	/* Power up the flash driver and reset it */
	ret = flashnode_s_power(&flash->sd, 1);
	if (ret < 0)
		return ret;

	/* Setup default values. This makes sure that the chip is in a known
	 * state.
	 */
	ret = flashnode_setup(flash); //Now Not-ready
	if (ret < 0)
		goto fail;

	dev_dbg(&client->dev, "Successfully detected flashnode LED flash\n");
	flashnode_s_power(&flash->sd, 0);
	return 0;

fail:
	flashnode_s_power(&flash->sd, 0);
	return ret;
}

static int flashnode_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return flashnode_s_power(sd, 1);
}

static int flashnode_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return flashnode_s_power(sd, 0);
}

static const struct v4l2_subdev_internal_ops flashnode_internal_ops = {
	.registered = flashnode_detect,
	.open = flashnode_open,
	.close = flashnode_close,
};

static int flashnode_gpio_init(struct i2c_client *client)
{
	int ret;
	switch (Read_PROJ_ID()) {
		case PROJ_ID_ZX550ML:
			switch (Read_HW_ID()) {
				case HW_ID_EVB:
				case HW_ID_SR1:
				case HW_ID_SR2:
				case HW_ID_ER:
				case HW_ID_ER1_1:
				case HW_ID_pre_PR:
				case HW_ID_PR:
				case HW_ID_MP:
				case HW_ID_MP_SD:
					printk("flashnode --> HW_ID = 0x%x\n", Read_HW_ID());
					printk("flashnode --> PMIC GPIO4CTLO_REG pull high\n");
					ret = intel_scu_ipc_iowrite8(GPIO4CTLO_REG, 0x31);
					if(ret){
						printk(KERN_ALERT "flashnode --> Failed to output PMIC GPIO4 HIGH\n");
						return ret;
					}
					break;
				default:
					printk("flashnode --> HW_ID does not define\n");
					break;
			}
			break;
		default:
			printk("DISABLE FLASH FOR OTHER PROJECT in ZX550ML\n");
			printk("Project ID is not defined\n");
			break;
	}//end switch
	return 0;
}

static int flashnode_gpio_uninit(struct i2c_client *client)
{
	int ret;
	switch (Read_PROJ_ID()) {
		case PROJ_ID_ZX550ML:
			switch (Read_HW_ID()) {
				case HW_ID_EVB:
				case HW_ID_SR1:
				case HW_ID_SR2:
				case HW_ID_ER:
				case HW_ID_ER1_1:
				case HW_ID_pre_PR:
				case HW_ID_PR:
				case HW_ID_MP:
				case HW_ID_MP_SD:
					printk("flashnode --> HW_ID = 0x%x\n", Read_HW_ID());
					printk("flashnode --> PMIC GPIO4CTLO_REG pull low\n");
					ret = intel_scu_ipc_iowrite8(GPIO4CTLO_REG, 0x30);
					if(ret){
						printk(KERN_ALERT "Failed to output PMIC GPIO4 LOW\n");
						return ret;
					}
					break;
				default:
					printk("flashnode --> HW_ID does not define\n");
					break;
				}
			break;
		default:
			printk("DISABLE FLASH FOR CES in ZX550ML\n");
			printk("Project ID is not defined\n");
			break;
	}//end switch
	return 0;
}


static void flt_do_work_function(struct work_struct *dat)
{
	struct flashnode *flash;
	int value;
	int ret;

	if(inner_flashnode==NULL) return;
	flash = inner_flashnode;

	pr_info("[%s] flashnode_interrupt_handler = %d\n", FLASHNODE_NAME,flash->irq);
	ret = regmap_read(flash->map,FLASHNODE_FAULT,&value);
/*
#define FLASHNODE_VINMONEX		0x80
#define FLASHNODE_SC			0x40
#define FLASHNODE_OC			0x20
#define FLASHNODE_OTMP			0x10
#define FLASHNODE_FLED2			0x0C
#define FLASHNODE_FLED1			0x03
*/

	if(value & FLASHNODE_VINMONEX)
	    pr_info("[%s] error flashnode FLASHNODE_VINMONEX Fault\n", FLASHNODE_NAME);
	if(value & FLASHNODE_SC)
	    pr_info("[%s] error flashnode FLASHNODE_SC Fault\n", FLASHNODE_NAME);
	if(value & FLASHNODE_OC)
	    pr_info("[%s] error flashnode FLASHNODE_OC Fault\n", FLASHNODE_NAME);
	if(value & FLASHNODE_OTMP)
	    pr_info("[%s] error flashnode FLASHNODE_OTMP Fault\n", FLASHNODE_NAME);
	if(value & FLASHNODE_FLED2)
	    pr_info("[%s] error flashnode FLASHNODE_FLED2 Fault\n", FLASHNODE_NAME);
	if(value & FLASHNODE_FLED1)
	    pr_info("[%s] error flashnode FLASHNODE_FLED1 Fault\n", FLASHNODE_NAME);

}


static irqreturn_t flashnode_interrupt_handler(int irq, void *sd)
{
	queue_delayed_work(flt_wq, &flt_sensor_dowork, msecs_to_jiffies(0));
	return IRQ_HANDLED;
}

static int set_irq(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct flashnode *flash = to_flashnode(sd);
	int rc = 0 ;

	pr_info("[%s] flashnode FLED_DRIVER_FLT# gpio = %d\n", FLASHNODE_NAME,flash->gpio);
	flash->irq = gpio_to_irq(flash->gpio);
	pr_info("[%s] flashnode FLED_DRIVER_FLT# irq = %d\n", FLASHNODE_NAME,flash->irq);
	rc = request_irq(flash->irq,flashnode_interrupt_handler,
			IRQF_SHARED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING,"flashnode_irq",flash);
	if (rc<0) {
		pr_info("[%s] Could not register for flashnode interrupt, irq = %d, rc = %d\n", FLASHNODE_NAME,flash->irq,rc);
		rc = -EIO;
		goto err_gpio_request_irq_fail ;
	}

	enable_irq_wake(flash->irq);

	return 0;

err_gpio_request_irq_fail:
	return rc;
}

static int flashnode_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err;
	struct flashnode *flash;
	struct proc_dir_entry* proc_entry_flash;
	struct proc_dir_entry* proc_entry_camera;
	void* dummy = NULL;

	camera_open = 0;

	if (client->dev.platform_data == NULL) {
		dev_err(&client->dev, "no platform data\n");
		return -ENODEV;
	}

	flash = kzalloc(sizeof(*flash), GFP_KERNEL);
	if (!flash) {
		dev_err(&client->dev, "out of memory\n");
		return -ENOMEM;
	}

	flash->pdata = client->dev.platform_data;

	v4l2_i2c_subdev_init(&flash->sd, client, &flashnode_ops);//Now
	flash->sd.internal_ops = &flashnode_internal_ops;
	flash->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	flash->mode = FLASHNODE_MODE_SHUTDOWN;


	err = media_entity_init(&flash->sd.entity, 0, NULL, 0);
	if (err) {
		dev_err(&client->dev, "error initialize a media entity.\n");
		goto fail1;
	}

	flash->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_FLASH;
	mutex_init(&flash->power_lock);
	err = flashnode_gpio_init(client);
	if (err) {
		dev_err(&client->dev, "gpio request/direction_output fail");
		goto fail2;
	}

//Add node for flash control
	proc_entry_flash = proc_create_data("driver/asus_flash_brightness", 0666, NULL, &flash_proc_fops, dummy);
	proc_set_user(proc_entry_flash, 1000, 1000);
	inner_flashnode = flash;


	proc_entry_camera = proc_create_data("driver/camera_open", 0660, NULL, &camera_proc_fops, dummy);
	proc_set_user(proc_entry_camera, 1000, 1000);

	switch (Read_PROJ_ID()) {
		case PROJ_ID_ZX550ML:
			switch (Read_HW_ID()) {
				case HW_ID_EVB:
				case HW_ID_SR1:
				case HW_ID_SR2:
				case HW_ID_ER:
				case HW_ID_ER1_1:
				case HW_ID_pre_PR:
				case HW_ID_PR:
				case HW_ID_MP:
				case HW_ID_MP_SD:
					printk("flashnode --> HW_ID = 0x%x\n", Read_HW_ID());
					break;
				default:
					printk("flashnode --> HW_ID does not define\n");
					break;
			}
			break;
		default:
			printk("DISABLE FLASH FOR OTHER \n");
			pr_info("Project ID is not defined\n");
			break;
		}//end switch


	//set irq
	err = set_irq(client);
	if (err < 0)
		goto fail_for_irq;

	flt_wq = create_singlethread_workqueue("flto_wq");
	INIT_DELAYED_WORK(&flt_sensor_dowork, flt_do_work_function);
	return 0;

fail_for_irq:
fail2:
	media_entity_cleanup(&flash->sd.entity);
fail1:
	v4l2_device_unregister_subdev(&flash->sd);
	kfree(flash);
	return err;
}

static int flashnode_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct flashnode *flash = to_flashnode(sd);
	int ret;

	media_entity_cleanup(&flash->sd.entity);
	v4l2_device_unregister_subdev(sd);

	inner_flashnode=NULL;

	ret = flashnode_gpio_uninit(client);
	if (ret < 0)
		goto fail;

	kfree(flash);

    	disable_irq(flash->irq);
	gpio_free(flash->gpio);
        destroy_workqueue(flt_wq);

	return 0;
fail:
	dev_err(&client->dev, "gpio request/direction_output fail");
	return ret;
}

static const struct i2c_device_id flashnode_ids[] = {
	{FLASHNODE_NAME, 0},
	{ },
};
MODULE_DEVICE_TABLE(i2c, flashnode_ids);

static const struct dev_pm_ops flashnode_pm_ops = {
	.suspend = flashnode_suspend,
	.resume = flashnode_resume,
};

static struct i2c_driver flashnode_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = FLASHNODE_NAME,
		.pm   = &flashnode_pm_ops,
	},
	.probe = flashnode_probe,
	.remove = flashnode_remove,
	.id_table = flashnode_ids,
};

static __init int init_flashnode(void)
{
	int ret;
	switch (Read_PROJ_ID()) {
		case PROJ_ID_ZX550ML:
			ret = i2c_add_driver(&flashnode_driver);
			break;
		default:
			ret = -1;
			break;
	}
	return ret;
}

static __exit void exit_flashnode(void)
{
	i2c_del_driver(&flashnode_driver);
}

module_init(init_flashnode);
module_exit(exit_flashnode);
MODULE_AUTHOR("Chung-Yi Chou <chung-yi_chou@asus.com>");
MODULE_DESCRIPTION("FLASHNODE LED Flash Driver");
MODULE_LICENSE("GPL");

