#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x4efedfdd, "module_layout" },
	{ 0x4eaff2f5, "cdev_del" },
	{ 0x82ee90dc, "timer_delete_sync" },
	{ 0xc6f46339, "init_timer_key" },
	{ 0x9aec981b, "class_destroy" },
	{ 0xfa044dc9, "device_destroy" },
	{ 0xa2f4d4fb, "cdev_add" },
	{ 0x904b4dd5, "cdev_init" },
	{ 0x29964e26, "device_create" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x8fec7ad1, "__class_create" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0xc38c83b8, "mod_timer" },
	{ 0x15ba50a6, "jiffies" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0xc987d4ce, "send_sig" },
	{ 0xdbba4a3c, "__put_task_struct" },
	{ 0x8e9141c1, "get_task_mm" },
	{ 0x2d5f69b3, "rcu_read_unlock_strict" },
	{ 0xeb981336, "pid_task" },
	{ 0x74cea3a6, "find_vpid" },
	{ 0xf2ca48ad, "mmput" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0x37a0cba, "kfree" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x92997ed8, "_printk" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x9166fada, "strncpy" },
	{ 0x225cd02c, "kmem_cache_alloc_trace" },
	{ 0xb0c41fd, "kmalloc_caches" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0xbdfb6dbb, "__fentry__" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "65ABFF36D6FCF7E714F8D40");
