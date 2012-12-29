#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/spi/mcp23s08.h>
#include <linux/platform_device.h>

struct mcpload_entry {
	struct i2c_client *i2c_client;
	struct mcp23s08_platform_data i2c_pdata;
	struct platform_device *led_pdev;
	struct gpio_led_platform_data led_pdata;
	struct gpio_led led_array[16];
	char led_names[16][20];
	struct list_head list;
};

static struct list_head entries = LIST_HEAD_INIT (entries);
static DEFINE_MUTEX (entries_mutex);

static ssize_t mcpload_info (
	struct kobject *kobj, struct kobj_attribute *attr, 
	char *buf
)
{
	const char *msg = "MCP23008/MCP23017 Loader\n"
		"Usage: <adapter-number> <device-number> <gpio-base> <gpio-len> [<pullups>] [<leds:bool>]\n";
	memcpy (buf, msg, strlen (msg));
	return strlen (msg);
}

static void mcpload_null_release (struct device *dev) {}

static int mcpload_load_leds (struct mcpload_entry *entry, int gpiobase, int gpiolen)
{
	int i, ret;

	struct platform_device *pdev = entry->led_pdev = kzalloc (sizeof (*pdev), GFP_KERNEL);
	if (!pdev)
		return -ENOMEM;

	for (i = 0; i < gpiolen; i++)
	{
		int gpio = gpiobase + i;
		sprintf (entry->led_names[i], "mcp:%d", gpio);
		entry->led_array[i] = (struct gpio_led){
			.name = entry->led_names[i],
			.default_trigger = "none",
			.gpio = gpio,
			.active_low = 0,
			.retain_state_suspended = 1,
			.default_state = LEDS_GPIO_DEFSTATE_KEEP,
		};
	}

	entry->led_pdata = (struct gpio_led_platform_data){
		.num_leds = gpiolen,
		.leds = entry->led_array,
	};

	*pdev = (struct platform_device){
		.name = "leds-gpio",
		.id = PLATFORM_DEVID_AUTO,
		.dev.platform_data = &(entry->led_pdata),
		.dev.release = mcpload_null_release,
	};


	ret = platform_device_register (pdev);
	if (ret < 0)
		printk (KERN_ERR KBUILD_MODNAME ": failed to initialize LED device\n");
	else
		printk (KERN_INFO KBUILD_MODNAME ": LED initialized\n");

	return ret;
}

static ssize_t mcpload_load (
	struct kobject *kobj, struct kobj_attribute *attr,
	const char *buf, size_t count
)
{
	unsigned int adapnum, devnum, gpiobase, gpiolen, pullups = 0, leds = 0;
	if (sscanf (buf, "%i %i %i %i %i %i", &adapnum, &devnum, &gpiobase, &gpiolen, &pullups, &leds) < 4)
		return -EINVAL;

	// do some validation
	const char *drvtype;
	switch (gpiolen)
	{
		case 8:
			drvtype = "mcp23008";
			break;
		case 16:
			drvtype = "mcp23017";
			break;
		default:
			printk (KERN_ERR KBUILD_MODNAME ": gpio length must be 8 or 16\n");
			return -EINVAL;
	}

	// get the adapter
	struct i2c_adapter *adapter = i2c_get_adapter (adapnum);
	if (!adapter)
	{
		printk (KERN_ERR KBUILD_MODNAME ": adapter %d not found\n", adapnum);
		return -ENODEV;
	}

	ssize_t ret;
	struct mcpload_entry *entry = kzalloc (sizeof (*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	// build platform data
	struct mcp23s08_platform_data *pdata = &(entry->i2c_pdata);
	pdata->base = gpiobase;
	pdata->chip[0].pullups = pullups;

	// populate the info
	struct i2c_board_info info;
	memset (&info, 0, sizeof (info));
	strcpy (info.type, drvtype);
	info.addr = devnum;
	info.platform_data = pdata;

	// add the device
	struct i2c_client *client = entry->i2c_client = i2c_new_device (adapter, &info);
	if (!client)
	{
		printk (KERN_ERR KBUILD_MODNAME ": failed to load driver\n");
		ret = -EINVAL;
		goto err1;
	}

	// add leds
	if (leds)
	{
		ret = mcpload_load_leds (entry, gpiobase, gpiolen);
		if (ret < 0)
			goto err2;
	}

	// we are successful
	mutex_lock (&entries_mutex);
	list_add_tail (&(entry->list), &(entries));
	mutex_unlock (&entries_mutex);

	printk (KERN_INFO KBUILD_MODNAME ": device at adapter %d device 0x%02x instantiated\n", adapnum, devnum);
	return count;

err2:
	i2c_unregister_device (client);
err1:
	kfree (entry);
	return ret;
}

static struct kobj_attribute attr = __ATTR (load, 0644, mcpload_info, mcpload_load);
static struct module *module;
static struct kobject *kobject;

static int mcpload_init (void)
{
	module = find_module (KBUILD_MODNAME);
	kobject = &(module->mkobj.kobj);

	sysfs_create_file (kobject, &(attr.attr));
	
	return 0;
}

static void mcpload_exit (void)
{
	sysfs_remove_file (kobject, &(attr.attr));

	mutex_lock (&entries_mutex);
	struct mcpload_entry *pos, *tmp;
	list_for_each_entry_safe (pos, tmp, &entries, list)
	{
		if (pos->led_pdev)
		{
			platform_device_unregister (pos->led_pdev);
			kfree (pos->led_pdev);
		}

		list_del (&(pos->list));
		i2c_unregister_device (pos->i2c_client);
		kfree (pos);
	}
	mutex_unlock (&entries_mutex);
}

module_init (mcpload_init);
module_exit (mcpload_exit);

MODULE_LICENSE ("GPL");
