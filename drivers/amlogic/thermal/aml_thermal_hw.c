#include <linux/amlogic/saradc.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/amlogic/cpu_version.h>
#include <linux/amlogic/scpi_protocol.h>
#include <linux/printk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/cpucore_cooling.h>
#include <linux/gpucore_cooling.h>
#include <linux/gpu_cooling.h>
#include <linux/thermal_core.h>
#include <linux/opp.h>
#include <linux/cpu.h>

#define NOT_WRITE_EFUSE		0x0
#define EFUEE_PRIVATE		0x4
#define EFUSE_OPS		0xa

enum cluster_type {
	CLUSTER_BIG = 0,
	CLUSTER_LITTLE,
	NUM_CLUSTERS
};

enum cool_dev_type {
	COOL_DEV_TYPE_CPU_FREQ = 0,
	COOL_DEV_TYPE_CPU_CORE,
	COOL_DEV_TYPE_GPU_FREQ,
	COOL_DEV_TYPE_GPU_CORE,
	COOL_DEV_TYPE_MAX
};

struct cool_dev {
	int min_state;
	int coeff;
	int cluster_id;
	char *device_type;
	struct device_node *np;
	struct thermal_cooling_device *cooling_dev;
};

struct aml_thermal_sensor {
	int chip_trimmed;
	int cool_dev_num;
	int min_exist;
	struct cpumask mask[NUM_CLUSTERS];
	struct cool_dev *cool_devs;
	struct thermal_zone_device    *tzd;
};

static struct aml_thermal_sensor soc_sensor;

int thermal_firmware_init(void)
{
	int ret;

	ret = scpi_get_sensor("aml_thermal");
	soc_sensor.chip_trimmed = ret < 0 ? 0 : 1;
	return ret;
}
EXPORT_SYMBOL(thermal_firmware_init);

int get_cpu_temp(void)
{
	unsigned int val = 0;

	if (soc_sensor.chip_trimmed) {	/* only supported trimmed chips */
		if (scpi_get_sensor_value(0, &val) < 0)
			return -1000;
		return val;
	} else {
		return -1000;
	}
}
EXPORT_SYMBOL(get_cpu_temp);

static int get_cur_temp(void *data, long *temp)
{
	int val;
	val = get_cpu_temp();
	if (val == -1000)
		return -EINVAL;

	*temp = val * 1000;

	return 0;
}

static int get_cool_dev_type(char *type)
{
	if (!strcmp(type, "cpufreq"))
		return COOL_DEV_TYPE_CPU_FREQ;
	if (!strcmp(type, "cpucore"))
		return COOL_DEV_TYPE_CPU_CORE;
	if (!strcmp(type, "gpufreq"))
		return COOL_DEV_TYPE_GPU_FREQ;
	if (!strcmp(type, "gpucore"))
		return COOL_DEV_TYPE_GPU_CORE;
	return COOL_DEV_TYPE_MAX;
}

static struct cool_dev *get_cool_dev_by_node(struct device_node *np)
{
	int i;
	struct cool_dev *dev;

	if (!np)
		return NULL;
	for (i = 0; i < soc_sensor.cool_dev_num; i++) {
		dev = &soc_sensor.cool_devs[i];
		if (dev->np == np)
			return dev;
	}
	return NULL;
}

int aml_thermal_min_update(struct thermal_cooling_device *cdev)
{
	struct gpufreq_cooling_device *gf_cdev;
	struct gpucore_cooling_device *gc_cdev;
	struct thermal_instance *ins;
	struct cool_dev *cool;
	long min_state;
	int i;
	int cpu, c_id;

	cool = get_cool_dev_by_node(cdev->np);
	if (!cool)
		return -ENODEV;

	if (cool->cooling_dev == NULL)
		cool->cooling_dev = cdev;

	if (cool->min_state == 0)
		return 0;

	switch (get_cool_dev_type(cool->device_type)) {
	case COOL_DEV_TYPE_CPU_CORE:
		/* TODO: cluster ID */
		cool->cooling_dev->ops->get_max_state(cdev, &min_state);
		min_state = min_state - cool->min_state;
		break;

	case COOL_DEV_TYPE_CPU_FREQ:
		for_each_possible_cpu(cpu) {
			if (mc_capable())
				c_id = topology_physical_package_id(cpu);
			else
				c_id = 0; /* force cluster 0 if no MC */
			if (c_id == cool->cluster_id)
				break;
		}
		min_state = cpufreq_cooling_get_level(cpu, cool->min_state);
		break;

	case COOL_DEV_TYPE_GPU_CORE:
		gc_cdev = (struct gpucore_cooling_device *)cdev->devdata;
		cdev->ops->get_max_state(cdev, &min_state);
		min_state = min_state - cool->min_state;
		break;

	case COOL_DEV_TYPE_GPU_FREQ:
		gf_cdev = (struct gpufreq_cooling_device *)cdev->devdata;
		min_state = gf_cdev->get_gpu_freq_level(cool->min_state);
		break;

	default:
		return -EINVAL;
	}

	for (i = 0; i < soc_sensor.tzd->trips; i++) {
		ins = get_thermal_instance(soc_sensor.tzd,
					   cdev, i);
		if (ins)
			ins->upper = min_state;
	}

	return 0;
}
EXPORT_SYMBOL(aml_thermal_min_update);

static struct thermal_zone_of_device_ops aml_thermal_ops = {
	.get_temp = get_cur_temp,
};

static int register_cool_dev(struct cool_dev *cool)
{
	int pp;
	int id = cool->cluster_id;
	struct cpumask *mask;

	switch (get_cool_dev_type(cool->device_type)) {
	case COOL_DEV_TYPE_CPU_CORE:
		cool->cooling_dev = cpucore_cooling_register(cool->np,
							     cool->cluster_id);
		break;

	case COOL_DEV_TYPE_CPU_FREQ:
		mask = &soc_sensor.mask[id];
		cool->cooling_dev = of_cpufreq_power_cooling_register(cool->np,
							mask,
							cool->coeff,
							NULL);
		break;

	/* GPU is KO, just save these parameters */
	case COOL_DEV_TYPE_GPU_FREQ:
		if (of_property_read_u32(cool->np, "num_of_pp", &pp))
			pr_err("thermal: read num_of_pp failed\n");
		save_gpu_cool_para(cool->coeff, cool->np, pp);
		return 0;

	case COOL_DEV_TYPE_GPU_CORE:
		save_gpucore_thermal_para(cool->np);
		return 0;

	default:
		pr_err("thermal: unknown type:%s\n", cool->device_type);
		return -EINVAL;
	}

	if (IS_ERR(cool->cooling_dev)) {
		pr_err("thermal: register %s failed\n", cool->device_type);
		return -EINVAL;
	}
	return 0;
}

static int parse_cool_device(struct device_node *np)
{
	int i, temp, ret = 0;
	struct cool_dev *cool;
	struct device_node *node, *child;
	const char *str;

	child = of_get_next_child(np, NULL);
	for (i = 0; i < soc_sensor.cool_dev_num; i++) {
		cool = &soc_sensor.cool_devs[i];
		if (child == NULL)
			break;
		if (of_property_read_u32(child, "min_state", &temp))
			pr_err("thermal: read min_state failed\n");
		else
			cool->min_state = temp;

		if (of_property_read_u32(child, "dyn_coeff", &temp))
			pr_err("thermal: read dyn_coeff failed\n");
		else
			cool->coeff = temp;

		if (of_property_read_u32(child, "cluster_id", &temp))
			pr_err("thermal: read cluster_id failed\n");
		else
			cool->cluster_id = temp;

		if (of_property_read_string(child, "device_type", &str))
			pr_err("thermal: read device_type failed\n");
		else
			cool->device_type = (char *)str;

		if (of_property_read_string(child, "node_name", &str))
			pr_err("thermal: read node_name failed\n");
		else {
			node = of_find_node_by_name(NULL, str);
			if (!node)
				pr_err("thermal: can't find node\n");
			cool->np = node;
		}
		if (cool->np)
			ret += register_cool_dev(cool);
		child = of_get_next_child(np, child);
	}
	return ret;
}

static int aml_thermal_probe(struct platform_device *pdev)
{
	int cpu, i, c_id;
	struct device_node *np, *child;
	struct cool_dev *cool;

	memset(&soc_sensor, 0, sizeof(struct aml_thermal_sensor));
	if (!cpufreq_frequency_get_table(0)) {
		dev_info(&pdev->dev,
			"Frequency table not initialized. Deferring probe...\n");
		return -EPROBE_DEFER;
	}

	if (thermal_firmware_init() < 0) {
		dev_err(&pdev->dev, "chip is not trimmed, disable thermal\n");
		return -EINVAL;
	}

	for_each_possible_cpu(cpu) {
		if (mc_capable())
			c_id = topology_physical_package_id(cpu);
		else
			c_id = CLUSTER_BIG;	/* Always cluster 0 if no mc */
		if (c_id > NUM_CLUSTERS) {
			pr_err("Cluster id: %d > %d\n", c_id, NUM_CLUSTERS);
			return -EINVAL;
		}
		cpumask_set_cpu(cpu, &soc_sensor.mask[c_id]);
	}

	np = pdev->dev.of_node;
	child = of_get_child_by_name(np, "cooling_devices");
	if (child == NULL) {
		pr_err("thermal: can't found cooling_devices\n");
		return -EINVAL;
	}
	soc_sensor.cool_dev_num = of_get_child_count(child);
	i = sizeof(struct cool_dev) * soc_sensor.cool_dev_num;
	soc_sensor.cool_devs = kzalloc(i, GFP_KERNEL);
	if (soc_sensor.cool_devs == NULL) {
		pr_err("thermal: alloc mem failed\n");
		return -ENOMEM;
	}

	if (parse_cool_device(child))
		return -EINVAL;

	soc_sensor.tzd = thermal_zone_of_sensor_register(&pdev->dev,
							  3,
							  &soc_sensor,
							  &aml_thermal_ops);

	if (IS_ERR(soc_sensor.tzd)) {
		dev_warn(&pdev->dev, "Error registering sensor: %p\n",
			 soc_sensor.tzd);
		return PTR_ERR(soc_sensor.tzd);
	}
	thermal_zone_device_update(soc_sensor.tzd);

	/* update min state for each device */
	for (i = 0; i < soc_sensor.cool_dev_num; i++) {
		cool = &soc_sensor.cool_devs[i];
		if (cool->cooling_dev)
			aml_thermal_min_update(cool->cooling_dev);
	}
	thermal_zone_device_update(soc_sensor.tzd);

	return 0;
}

static int aml_thermal_remove(struct platform_device *pdev)
{
	kfree(soc_sensor.cool_devs);
	return 0;
}

static struct of_device_id aml_thermal_of_match[] = {
	{ .compatible = "amlogic, aml-thermal" },
	{},
};

static struct platform_driver aml_thermal_platdrv = {
	.driver = {
		.name		= "aml-thermal",
		.owner		= THIS_MODULE,
		.of_match_table = aml_thermal_of_match,
	},
	.probe	= aml_thermal_probe,
	.remove	= aml_thermal_remove,
};


static int __init aml_thermal_platdrv_init(void)
{
	 return platform_driver_register(&(aml_thermal_platdrv));
}
late_initcall(aml_thermal_platdrv_init);