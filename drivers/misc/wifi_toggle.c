/*
 * Author: Hadidjapri
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "wifi_toggle.h"


int wifi_pm;

static ssize_t wifi_pm_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", wifi_pm);
}


static ssize_t wifi_pm_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	sscanf(buf, "%d", &wifi_pm);
	return count;
}


static struct kobj_attribute wifi_pm_attribute =
__ATTR(wifi_pm, 0666, wifi_pm_show, wifi_pm_store);

static struct attribute *wifi_pm_attrs[] = {
&wifi_pm_attribute.attr,
NULL,
};

static struct attribute_group wifi_pm_attr_group = {
.attrs = wifi_pm_attrs,
};

static struct kobject *wifi_pm_kobj;

int wifi_pm_init(void)
{
	int wifi_pm_retval;

        wifi_pm_kobj = kobject_create_and_add("wifi_pm", kernel_kobj);

        if (!wifi_pm_kobj) {
                return -ENOMEM;
        }

        wifi_pm_retval = sysfs_create_group(wifi_pm_kobj, &wifi_pm_attr_group);

        if (wifi_pm_retval)
			kobject_put(wifi_pm_kobj);
	
	wifi_pm = 0;

        return (wifi_pm_retval);
}


void wifi_pm_exit(void)
{
	kobject_put(wifi_pm_kobj);
}


module_init(wifi_pm_init);
module_exit(wifi_pm_exit);
