/*
 * Copyright (c) 2010, Facebook, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * Neither the name Facebook nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <ctype.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <flashcache.h>

#undef COMMIT_REV

void
usage(char *pname)
{
	fprintf(stderr, "Usage: %s [-v] [-p back|thru|around] [-b block size] [-m md block size] [-s cache size] [-a associativity] cachedev ssd_devname disk_devname\n", pname);
	fprintf(stderr, "Usage : %s Cache Mode back|thru|around is required argument\n",
		pname);
	fprintf(stderr, "Usage : %s Default units for -b, -m, -s are sectors, or specify in k/M/G. Default associativity is 512.\n",
		pname);
#ifdef COMMIT_REV
	fprintf(stderr, "git commit: %s\n", COMMIT_REV);
#endif
	exit(1);
}

char *pname;
char buf[512], nvram_buf[512];//新增 nvram_buf
char dmsetup_cmd[8192];
int verbose = 0;
int force = 0;

static sector_t
get_block_size(char *s)
{
	sector_t size;
	char *c;
	
	size = strtoll(s, NULL, 0);
	for (c = s; isdigit(*c); c++)
		;
	switch (*c) {
		case '\0': 
			break;
		case 'k':
			size = (size * 1024) / 512;
			break;
		default:
			fprintf (stderr, "%s: Unknown block size type %c\n", pname, *c);
			exit (1);
	}
	if (size & (size - 1)) {
		fprintf(stderr, "%s: Block size must be a power of 2\n", pname);
		exit(1);
	}
	return size;
}

static sector_t
get_cache_size(char *s)
{
	sector_t size;
	char *c;
	
	size = strtoll(s, NULL, 0);
	for (c = s; isdigit (*c); c++)
		;
	switch (*c) {
		case '\0': 
			break;
		case 'k':
			size = (size * 1024) / 512;
			break;
		case 'm':
		case 'M':
			size = (size * 1024 * 1024) / 512;
			break;
		case 'g': 
		case 'G': 
			size = (size * 1024 * 1024 * 1024) / 512;
			break;
		case 't': 
		case 'T': 
			/* Cache size in terabytes?  You lucky people! */
			size = (size * 1024 * 1024 * 1024 * 1024) / 512;
			break;
		default:
			fprintf (stderr, "%s: Unknown cache size type %c\n", pname, *c);
			exit (1);
	}
	return size;
}

//判断flashcache模块是否已经加载   
// /proc/modules中记录模块信息  读取该文件、判断是否存在flashcache字符串
static int 
module_loaded(void)
{
	FILE *fp;
	char line[8192];
	int found = 0;
	
	fp = fopen("/proc/modules", "ro");
	while (fgets(line, 8190, fp)) {
		char *s;
		
		s = strtok(line, " ");
		if (!strcmp(s, "flashcache")) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static void
load_module(void)
{
	FILE *fp;
	char line[8192];
	//若没有加载则调用system执行modprobe flashcache加载模块
	if (!module_loaded()) {
		if (verbose)
			fprintf(stderr, "Loading Flashcache Module\n");
		system("modprobe flashcache");
		if (!module_loaded()) {
			fprintf(stderr, "Could not load Flashcache Module\n");
			exit(1);
		}
	} else if (verbose)//verbose表示是否显示详细信息
			fprintf(stderr, "Flashcache Module already loaded\n");
	fp = fopen("/proc/flashcache/flashcache_version", "ro");
	fgets(line, 8190, fp);
	if (fgets(line, 8190, fp)) {
		if (verbose)
			fprintf(stderr, "version string \"%s\"\n", line);
#ifdef COMMIT_REV
		if (!strstr(line, COMMIT_REV)) {
			fprintf(stderr, "Flashcache revision doesn't match tool revision.\n");
			exit(1);
		}
#endif
	}
	fclose(fp);
}

static void 
check_sure(void)
{
	char input;

	fprintf(stderr, "Are you sure you want to proceed ? (y/n): ");
	scanf("%c", &input);
	printf("\n");
	if (input != 'y') {
		fprintf(stderr, "Exiting FlashCache creation\n");
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	int nvram_fd, cache_fd, disk_fd, c;//新增 nvram_fd
	char *disk_devname, *ssd_devname, *nvram_devname, *cachedev;//新增 nvram_devname表示nvram缓存路径名字
	struct flash_superblock *sb = (struct flash_superblock *)buf;//flash超级块的对象sb指向buf空间
	struct nvram_superblock *nb = (struct nvram_superblock *)nvram_buf;//新增 nb结构体
	sector_t nvram_cache_devsize, cache_devsize, disk_devsize;//新增 nvram_cache_devsize
	sector_t block_size = 0, md_block_size = 0, cache_size = 0, nvram_cache_size = 0;//新增 nvram_cache_size
	sector_t ram_needed;//typedef unsigned long sector_t;
	struct sysinfo i;
	int cache_sectorsize, nvram_cache_sectorsize;//新增 nvram的扇区大小
	int associativity = 512;
	int disk_associativity = 512;
	int ret;
	int cache_mode = -1;
	char *cache_mode_str;
	
	pname = argv[0];
	//flashcache_create -p back flashcache /dev/pma /dev/pmb /dev/loop0
	//flashcache_create [-v] -p back|around|thru [-s cache size] [-b block size] cachedevname ssd_devname disk_devname
	while ((c = getopt(argc, argv, "fs:b:d:m:va:p:")) != -1) {
		switch (c) {
		case 's':
			//这个位置以后还可以初始化nvram_cache_size属性
			cache_size = get_cache_size(optarg);//s选项后面跟着的是缓存大小 默认大小，不然就要分别指定两个缓存的大小
			break;
		case 'a':
			associativity = atoi(optarg);//缓存分组大小
			break;
		case 'b':
			block_size = get_block_size(optarg);//缓存块大小  2的n次幂
			/* Block size should be a power of 2 */
                        break;
		case 'd':
			disk_associativity = get_block_size(optarg);//磁盘分组大小
			break;
		case 'm':
			md_block_size = get_block_size(optarg);//元数据块大小
			/* MD block size should be a power of 2 */
                        break;
		case 'v':
			verbose = 1;
                        break;			
		case 'f':
			force = 1;
                        break;
		case 'p':
			if (strcmp(optarg, "back") == 0) {        //默认设置成WB模式
				cache_mode = FLASHCACHE_WRITE_BACK;
				cache_mode_str = "WRITE_BACK";
			} else if ((strcmp(optarg, "thru") == 0) ||
				   (strcmp(optarg, "through") == 0)) {
				cache_mode = FLASHCACHE_WRITE_THROUGH;
				cache_mode_str = "WRITE_THROUGH";
			} else if (strcmp(optarg, "around") == 0) {
				cache_mode = FLASHCACHE_WRITE_AROUND;
				cache_mode_str = "WRITE_AROUND";
			} else
				usage(pname);
                        break;			
		case '?':
			usage(pname);
		}
	}
	if (cache_mode == -1)
		usage(pname);
	if (optind == argc)
		usage(pname);
	if (block_size == 0)
		block_size = 8;		/* 4KB default blocksize */  //缓存块大小默认为8个扇区，4KB
	if (md_block_size == 0)
		md_block_size = 8;	/* 4KB default blocksize */  //元数据块大小也是一样
	//进一步分别获取虚拟设备、flash设备和磁盘设备的名字   如果说参数个数提前用完了optind==argc，则调用usage返回错误信息
	cachedev = argv[optind++];
	if (optind == argc)
		usage(pname);
	//新增 获取nvram的设备路径名
	nvram_devname = argv[optind++];
	if (optind == argc)
		usage(pname);
	ssd_devname = argv[optind++];
	if (optind == argc)
		usage(pname);
	disk_devname = argv[optind];
	//新增 对nvram路径名/磁盘分组大小的输出 
	printf("cachedev %s, nvram_devname %s, ssd_devname %s, disk_devname %s cache_mode %s disk_associativity %lu\n", 
	       cachedev, nvram_devname, ssd_devname, disk_devname, cache_mode_str, disk_associativity);
	if (cache_mode == FLASHCACHE_WRITE_BACK)
		printf("block_size %lu, md_block_size %lu, cache_size %lu\n", 
		       block_size, md_block_size, cache_size);
	else
		printf("block_size %lu, cache_size %lu\n", 
		       block_size, cache_size);

	//新增 读取nvram缓存空间的超级块中的数据，并判断nvram中是否已经有数据
	nvram_fd = open(nvram_devname, O_RDONLY);
	if (nvram_fd < 0) {
		fprintf(stderr, "Failed to open %s\n", nvram_devname);
		exit(1);
	}
    lseek(nvram_fd, 0, SEEK_SET);
	if (read(nvram_fd, nvram_buf, 512) < 0) {
		fprintf(stderr, "Cannot read NVRAM superblock %s\n", 
			nvram_devname);
		exit(1);		
	}
	if (nb->cache_sb_state == CACHE_MD_STATE_DIRTY ||
	    nb->cache_sb_state == CACHE_MD_STATE_CLEAN ||
	    nb->cache_sb_state == CACHE_MD_STATE_FASTCLEAN ||
	    nb->cache_sb_state == CACHE_MD_STATE_UNSTABLE) {
		fprintf(stderr, "%s: Valid NVRAM already exists on %s\n", 
			pname, nvram_devname);
		fprintf(stderr, "%s: Use flashcache_destroy first and then create again %s\n", 
			pname, nvram_devname);
		exit(1);
	}

	//读取flash缓存空间的超级块中的数据
	//指向文件的头部，并从头部开始读取512个字节的数据到buf中  那就应该是超级块只占用一个扇区大小？
	cache_fd = open(ssd_devname, O_RDONLY);
	if (cache_fd < 0) {
		fprintf(stderr, "Failed to open %s\n", ssd_devname);
		exit(1);
	}
    lseek(cache_fd, 0, SEEK_SET);
	if (read(cache_fd, buf, 512) < 0) {
		fprintf(stderr, "Cannot read Flashcache superblock %s\n", 
			ssd_devname);
		exit(1);		
	}
	//通过flash缓存超级块的clean或者shutdown状态来判断是否已经有缓存数据在flash中
	if (sb->cache_sb_state == CACHE_MD_STATE_DIRTY ||
	    sb->cache_sb_state == CACHE_MD_STATE_CLEAN ||
	    sb->cache_sb_state == CACHE_MD_STATE_FASTCLEAN ||
	    sb->cache_sb_state == CACHE_MD_STATE_UNSTABLE) {
		fprintf(stderr, "%s: Valid Flashcache already exists on %s\n", 
			pname, ssd_devname);
		fprintf(stderr, "%s: Use flashcache_destroy first and then create again %s\n", 
			pname, ssd_devname);
		exit(1);
	}
	//
	disk_fd = open(disk_devname, O_RDONLY);
	if (disk_fd < 0) {
		fprintf(stderr, "%s: Failed to open %s\n", 
			pname, disk_devname);
		exit(1);
	}

	//新增 获取nvram设备空间大小和物理扇区大小
	//并判断nvram缓存数据块大小是否大于nvram物理扇区大小和nvram缓存空间大小是否小于nvram设备大小
	//不过默认cache_size应该为0，所以nvram_cache_size也默认为0
	if (ioctl(nvram_fd, BLKGETSIZE, &nvram_cache_devsize) < 0) {
		fprintf(stderr, "%s: Cannot get nvram cache size %s\n", 
		pname, nvram_devname);
	exit(1);		
	}
	if (ioctl(nvram_fd, BLKSSZGET, &nvram_cache_sectorsize) < 0) {
		fprintf(stderr, "%s: Cannot get nvram cache size %s\n", 
			pname, nvram_devname);
		exit(1);		
	}
	if (md_block_size > 0 &&
	    md_block_size * 512 < nvram_cache_sectorsize) {
		fprintf(stderr, "%s: NVRAM device (%s) sector size (%d) cannot be larger than metadata block size (%d) !\n",
		        pname, nvram_devname, nvram_cache_sectorsize, md_block_size * 512);
		exit(1);				
	}
	if (nvram_cache_size && nvram_cache_size > nvram_cache_devsize) {
		fprintf(stderr, "%s: Cache size is larger than nvram size %lu/%lu\n", 
			pname, nvram_cache_size, nvram_cache_devsize);
		exit(1);		
	}

	//获取flash设备的空间大小并写入到cache_devsize中
	if (ioctl(cache_fd, BLKGETSIZE, &cache_devsize) < 0) {
		fprintf(stderr, "%s: Cannot get cache size %s\n", 
			pname, ssd_devname);
		exit(1);		
	}
	if (ioctl(disk_fd, BLKGETSIZE, &disk_devsize) < 0) {
		fprintf(stderr, "%s: Cannot get disk size %s\n", 
			pname, disk_devname);
		exit(1);				
	}

	//获取flash设备的物理扇区大小
	if (ioctl(cache_fd, BLKSSZGET, &cache_sectorsize) < 0) {
		fprintf(stderr, "%s: Cannot get cache size %s\n", 
			pname, ssd_devname);
		exit(1);		
	}
	//flash设备的物理扇区大小不能大于元数据块的大小   不然元数据块就成了管理flash的最小数据单元了
	if (md_block_size > 0 &&
	    md_block_size * 512 < cache_sectorsize) {
		fprintf(stderr, "%s: SSD device (%s) sector size (%d) cannot be larger than metadata block size (%d) !\n",
		        pname, ssd_devname, cache_sectorsize, md_block_size * 512);
		exit(1);				
	}
	//缓存空间大小不能大于flash设备空间大小
	if (cache_size && cache_size > cache_devsize) {
		fprintf(stderr, "%s: Cache size is larger than ssd size %lu/%lu\n", 
			pname, cache_size, cache_devsize);
		exit(1);		
	}

	/* Remind users how much core memory it will take - not always insignificant.
 	 * If it's > 25% of RAM, warn.
         */
 	//如果缓存空间大小没有在参数中赋值，为0，则为设备大小/块大小 *缓存块大小，即整个设备 
 	//新增 nvram_cache_size为0，ram_needed加上nvram设备大小
	if (cache_size == 0 || nvram_cache_size == 0)
		ram_needed = (cache_devsize / block_size) * sizeof(struct cacheblock) + (nvram_cache_devsize / block_size) * sizeof(struct cacheblock);	/* Whole device */
	else
		ram_needed = (cache_size / block_size) * sizeof(struct cacheblock) + (nvram_cache_size / block_size) * sizeof(struct cacheblock);

	sysinfo(&i);
	printf("Flashcache metadata will use %luMB of your %luMB main memory\n",
		ram_needed >> 20, i.totalram >> 20);
	//若是所使用的Flashcache元数据空间占内存空间比例超过1/4则提示     ram_needed不是整个缓存空间的大小吗？为什么说元数据？
	if (!force && ram_needed > (i.totalram * 25 / 100)) {
		fprintf(stderr, "Proportion of main memory needed for flashcache metadata is high.\n");
		fprintf(stderr, "You can reduce this with a smaller cache or a larger blocksize.\n");
		check_sure();
	}
	printf("输出一次disk_associativity=%lu associativity=%lu\n", disk_associativity, associativity);
	//磁盘分组大小不能大于缓存分组大小
	if (disk_associativity == 0 ||
	    disk_associativity > associativity) {
		fprintf(stderr, "%s: Invalid Disk Associativity %ld\n",
			pname, disk_associativity);
		exit(1);
	}
	printf("再输出一次disk_associativity=%lu associativity=%lu\n", disk_associativity, associativity);
	//缓存大小也不能大于磁盘大小   
	//新增 加入nvram_cache_size > disk_devsize
	if (!force && (cache_size > disk_devsize || nvram_cache_size > disk_devsize)) {
		fprintf(stderr, "Size of cache volume (%s) || (%s) is larger than disk volume (%s)\n",
			nvram_devname, ssd_devname, disk_devname);
		check_sure();
	}
	//新增  先提前输出一遍命令内容  共享cache_mode、block_size、assoc、md_block_size   persistence默认为2，即create
	printf("echo 0 %lu flashcache disk=%s ssd=%s nvram=%s cachedev=%s cachemode=%d 2 blocksize=%lu cachesize=%lu nvramsize=%lu assoc=%d diskassoc=%lu md_block_size=%lu"
		" | dmsetup create %s.\n",
		disk_devsize, disk_devname, ssd_devname, nvram_devname, cachedev, cache_mode, block_size, 
		cache_size, nvram_cache_size, associativity, disk_associativity, md_block_size,
		cachedev);
	printf("最后输出一次disk_associativity=%lu associativity=%lu\n", disk_associativity, associativity);

	/*
[root@localhost flashcache-3.1.3]# flashcache_create -p back cache1g8g /dev/pma /dev/pmb /dev/loop0
cachedev cache1g8g, nvram_devname /dev/pma, ssd_devname /dev/pmb, disk_devname /dev/loop0 cache mode WRITE_BACK
block_size 8, md_block_size 8, cache_size 0
Flashcache metadata will use 58MB of your 64426MB main memory
echo 0 20971520 flashcache disk=/dev/loop0 ssd=/dev/pmb nvram=/dev/pma cachedev=cache1g8g cachemode=1 2 blocksize=8 
cachesize=0 nvramsize=0 assoc=512 diskassoc=266287972864 md_block_size=8 | dmsetup create cache1g8g
	*/

	//设计创建设备的命令  先不加入nvram，不然后面需要加上解析参数的部分才能正常运行
	sprintf(dmsetup_cmd, "echo 0 %lu flashcache %s %s %s %d 2 %lu %lu %d %lu %lu"
		" | dmsetup create %s",
		disk_devsize, disk_devname, ssd_devname, cachedev, cache_mode, block_size, 
		cache_size, associativity, disk_associativity, md_block_size,
		cachedev);

	/* Go ahead and create the cache.
	 * XXX - Should use the device mapper library for this.
	 */
	//加载flashcache模块
	load_module();
	if (verbose)
		fprintf(stderr, "Creating FlashCache Volume : \"%s\"\n", dmsetup_cmd);
	//执行命令  创建设备     暂时先不创建混合设备
	ret = system(dmsetup_cmd);
	if (ret) {
		fprintf(stderr, "%s failed\n", dmsetup_cmd);
		exit(1);
	}
	return 0;
}
