#include <filesys/inode.h>
#include <device/ata.h>
#include <device/block.h>
#include <bitmap.h>
#include <device/console.h>
#include <string.h>

//Block 12~15 valid inode number = 256
//inode size = 64byte, block size = 4096byte, 64 inode/block

char tmpblock[SSU_BLOCK_SIZE];
char tmpblock_indirect[SSU_BLOCK_SIZE]; //4096
extern struct process *cur_process;

void init_inode_table(struct ssu_fs *fs)
{
	struct bitmap *imap = fs->fs_inodemap;
	int i;

	for(i=0; i<NUM_INODEBLOCK; i++)
		fs_readblock(fs, SSU_INODE_BLOCK + i, ((char*)inode_tbl) + (i * SSU_BLOCK_SIZE));	

	if(!bitmap_test(imap, INO_ROOTDIR))
	{
		memset(inode_tbl, 0, sizeof(struct inode) * NUM_INODE);
		//Unvalid, Reserved
		bitmap_set(imap, 0, true);
		bitmap_set(imap, 1, true);

		inode_tbl[0].sn_ino = 0;
		inode_tbl[0].sn_size = 0;
		inode_tbl[0].sn_type = SSU_TYPE_DIR;
		inode_tbl[0].sn_nlink = 0;
		inode_tbl[0].sn_refcount = 0;
		inode_tbl[0].sn_fs = fs;

		//Root directory set

		make_dir(inode_tbl, ".");
		sync_inode(fs, inode_tbl + INO_ROOTDIR);
		sync_bitmapblock(fs);
	}

	cur_process->rootdir = inode_tbl + INO_ROOTDIR;
	cur_process->cwd = cur_process->rootdir;
}

struct inode* inode_create(struct ssu_fs *fs, uint16_t type)
{
	struct inode *new_inode;
	struct bitmap *imap = fs->fs_inodemap;
	int idx;

	for(idx = 0; idx <NUM_INODE; idx++)
		if(!bitmap_test(imap, idx))
			break;

	if(idx >= NUM_INODE)
		return NULL;

	new_inode = inode_tbl + idx;
	new_inode->sn_ino = idx;
	new_inode->sn_size = 0;
	new_inode->sn_type = type;
	new_inode->sn_nlink = 0;
	new_inode->sn_refcount = 0;
	new_inode->sn_fs = fs;
	new_inode->cnt_data_block = 0;

	bitmap_set(imap, idx, true);

	sync_inode(fs, new_inode);
	sync_bitmapblock(fs);

	return new_inode;
}

int inode_write(struct inode *in, uint32_t offset, char * buf, int len)
{

	//modify the inode_write to activate indirect blocks.
	int result=0;
	struct ssu_fs * fs = in->sn_fs;
	struct bitmap *bmap = fs->fs_blkmap;
	uint32_t blkoff = offset / SSU_BLOCK_SIZE;
	uint32_t start_blkoff = blkoff;
	uint32_t res_off = offset % SSU_BLOCK_SIZE;
	int pbn;
	int lenlen = len;
	
	if(offset > in->sn_size)
		return -1;
	
	while (len > 0) {
		//피지컬 블럭 넘버 받아옴
		pbn = lbn_to_pbn(in, blkoff);
		memset(tmpblock, 0, SSU_BLOCK_SIZE);
		//블럭 읽기		
		fs_readblock(fs, pbn, tmpblock);
		//블럭버퍼 수정
		if (len >= SSU_BLOCK_SIZE - res_off) {
			memcpy(tmpblock + res_off, buf, SSU_BLOCK_SIZE - res_off);
			len -= SSU_BLOCK_SIZE - res_off;
			buf += SSU_BLOCK_SIZE - res_off;
		}
		else {
			memcpy(tmpblock + res_off, buf, len);
			len = 0;
		}
		//블럭버퍼 블럭에 복사
		fs_writeblock(fs, pbn, tmpblock);
		res_off = 0;
		blkoff++;
	}
	blkoff--;
	if(in->sn_size < offset+lenlen)
		in->sn_size = offset+lenlen;
	sync_inode(fs, in);
	
	printk("start blkoff = %d\n", start_blkoff);
	printk("end blkoff = %d\n", blkoff);
	printk("physical start blkoff = %d\n", lbn_to_pbn(in, start_blkoff));
	printk("physical end blkoff = %d\n", lbn_to_pbn(in, blkoff));
	return result;
	//
}

int inode_read(struct inode * in, uint32_t offset, char * buf, int len)
{

	//modify the inode_write to activate indirect blocks.
	int result=0;
	struct ssu_fs * fs = in->sn_fs;
	uint32_t blkoff = offset / SSU_BLOCK_SIZE;
	uint32_t start_blkoff = blkoff;
	uint32_t res_off = offset % SSU_BLOCK_SIZE;
	int pbn;
	if(offset > in->sn_size)
		return -1;
	
	while (len > 0) {
		//피지컬 블럭 넘버 받아옴
		pbn = lbn_to_pbn(in, blkoff);
		memset(tmpblock, 0, SSU_BLOCK_SIZE);
		//블럭 읽기		
		fs_readblock(fs, pbn, tmpblock);
		//블럭내용 버퍼에 저장
		if (len >= SSU_BLOCK_SIZE - res_off) {
			memcpy(buf, tmpblock + res_off, SSU_BLOCK_SIZE - res_off);
			len -= SSU_BLOCK_SIZE - res_off;
			buf += SSU_BLOCK_SIZE - res_off;
		}
		else {
			memcpy(buf, tmpblock + res_off, len);
			len = 0;
		}
		blkoff++;
		res_off = 0;
	}
	blkoff--;
	printk("start blkoff = %d\n", start_blkoff);
	printk("end blkoff = %d\n", blkoff);
	printk("physical start blkoff = %d\n", lbn_to_pbn(in, start_blkoff));
	printk("physical end blkoff = %d\n", lbn_to_pbn(in, blkoff));
	return result;

	//
}

int lbn_to_pbn(struct inode * in, uint32_t lbn )
{
	struct ssu_fs * fs = in->sn_fs;
	struct bitmap *bmap = fs->fs_blkmap;
	int pbn=0;
	int indirect_idx=0;
	int idx=0;
	int point_idx=0;
	if (lbn < 0)
		return -1;
	//complete this function for indirect block.
	//1이하면 다이렉트 블럭
	if (lbn < NUM_DIRECT)
	{
		//할당유무 체크 후 할당
		if (lbn >= in->sn_nlink)
		{	
			balloc(bmap, &(in->sn_directblock[lbn]));
			in->sn_nlink++;
			sync_bitmapblock(fs);
		}
		pbn = in->sn_directblock[lbn];
	}
	else
	{	
		//인다이렉트 배열 인덱스와 인다이렉트 블럭의 인덱스 
		indirect_idx = (lbn - NUM_DIRECT) / (SSU_BLOCK_SIZE / sizeof(uint32_t));
		idx = (lbn - NUM_DIRECT) % (SSU_BLOCK_SIZE / sizeof(uint32_t));

		if (indirect_idx >= NUM_INDIRECT)
			return -1;
		else if (indirect_idx >= in->cnt_data_block) {
			//인다이렉트 배열 블럭 할당
			balloc(bmap, &(in->sn_indirectblock[indirect_idx]));
			in->cnt_data_block++;
			sync_bitmapblock(fs);
		}
		
		//인다이렉트 블럭 읽기
		memset(tmpblock_indirect, 0, SSU_BLOCK_SIZE);
		fs_readblock(fs, in->sn_indirectblock[indirect_idx], tmpblock_indirect);
		memcpy(&pbn, &tmpblock_indirect[idx*sizeof(uint32_t)], sizeof(uint32_t));
		
		if(pbn == 0)//블럭이 할당 안돼있을 경우
		{
			//블럭 할당
			balloc(bmap, &tmpblock_indirect[idx*sizeof(uint32_t)]);
			fs_writeblock(fs, in->sn_indirectblock[indirect_idx], tmpblock_indirect);			
			sync_bitmapblock(fs);
		}
		
		memcpy(&pbn, &tmpblock_indirect[idx*sizeof(uint32_t)], sizeof(uint32_t));
		
	}

	return pbn;
}

struct inode *inode_open(const char *pathname)
{
	struct inode *inode_cur;
	struct inode *new_in;
	struct direntry buf_dir;
	char fname_buf[FILENAME_LEN];
	int ndir;
	int i;

	inode_cur = cur_process->cwd;
	for(i=0; pathname[i] != '\0'; i++)
		fname_buf[i] = pathname[i];
	fname_buf[i] = '\0';

	ndir = num_direntry(inode_cur);
	for(i=0; i<ndir; i++)
	{   
		inode_read(inode_cur, i*sizeof(struct direntry), (char *)&buf_dir, sizeof(struct direntry));
		if(strncmp(buf_dir.de_name, fname_buf,FILENAME_LEN) == 0) return &(inode_tbl[buf_dir.de_ino]); // 중복 이름 존재, 생성 없이 리턴
	}

	new_in = inode_create(cur_process->cwd->sn_fs,SSU_TYPE_FILE);
	buf_dir.de_ino = new_in->sn_ino;
	strlcpy(buf_dir.de_name, fname_buf, FILENAME_LEN);
	inode_write(inode_cur, num_direntry(inode_cur)*sizeof(buf_dir), (char *)&buf_dir, sizeof(buf_dir)); //상위 inode에 추가

	return new_in;
}


static int sync_inode(struct ssu_fs *fs, struct inode* inode)
{
	int result = 0;
	int offset = inode->sn_ino / INODE_PER_BLOCK; //offset to current inode index from SSU_INODE_BLOCK 

	result = fs_writeblock(fs, SSU_INODE_BLOCK + offset, (char*)(inode_tbl + offset*INODE_PER_BLOCK));
	return result;
}


int make_dir(struct inode *cwd, char *name)
{
	struct direntry newde, cde, pde, tmp;
	struct inode * newin;
	struct ssu_fs *fs = cwd->sn_fs;
	int ndir, i;
	int len = strnlen(name, FILENAME_LEN);

	if(len > FILENAME_LEN || len <= 0)
	{
		printk("Unvalid filename length.\n");
		return -1;	
	}

	if(cwd->sn_ino >= INO_ROOTDIR)
	{
		ndir = num_direntry(cwd);
		for(i=0; i<ndir; i++)
		{
			inode_read(cwd, i*sizeof(struct direntry), (char*)&tmp, sizeof(struct direntry));
			if( strncmp(name, tmp.de_name, len) == 0 && tmp.de_name[len] == 0)
			{
				printk("Already exist filename.\n");
				return -1;
			}
		}
	}

	newin = inode_create(fs, SSU_TYPE_DIR);

	newde.de_ino = newin->sn_ino;
	strlcpy(newde.de_name, name, len+1);


	if(cwd->sn_ino >= INO_ROOTDIR)
		inode_write(cwd, cwd->sn_size, (char*)&newde, sizeof(struct direntry));
	else
		cwd = newin;

	cde.de_ino = newde.de_ino;
	strlcpy(cde.de_name, ".", 2);

	pde.de_ino = cwd->sn_ino;
	strlcpy(pde.de_name, "..", 3);	

	inode_write(newin, newin->sn_size, (char*)&pde, sizeof(struct direntry));
	inode_write(newin, newin->sn_size, (char*)&cde, sizeof(struct direntry));

	return 0;
}

static int num_direntry(struct inode *in)
{
	if(in->sn_size % sizeof(struct direntry) != 0 || in->sn_type != SSU_TYPE_DIR)
		return -1;

	return in->sn_size / sizeof(struct direntry);
}


void list_segment(struct inode *cwd)
{
	int i;
	int ndir = num_direntry(cwd);
	struct inode *in;
	struct direntry de;

	printk("name | size | type | blocks | ino\n");
	for(i=0; i<ndir; i++)
	{
		inode_read(cwd, i*sizeof(struct direntry), (char*)&de, sizeof(struct direntry));
		in = &inode_tbl[de.de_ino];

		printk("%s | %d | %c | %d | %d\n", de.de_name, in->sn_size, 
				(in->sn_type == SSU_TYPE_DIR) ? 'd' : 'n', in->sn_nlink, in->sn_ino);
	}
}

int change_dir(struct inode *cwd, char *path)
{
	int i;
	int ndir = num_direntry(cwd);
	struct inode *in;
	struct direntry de;
	int len = strnlen(path, FILENAME_LEN);

	if(path == 0)
	{
		cur_process->cwd = cur_process->rootdir;
		return -1;
	}

	for(i=0; i<ndir; i++)
	{
		inode_read(cwd, i*sizeof(struct direntry), (char*)&de, sizeof(struct direntry));

		if( strncmp(path, de.de_name, len) == 0 && de.de_name[len] == 0)
		{
			in = &inode_tbl[de.de_ino];
			if(in->sn_type != SSU_TYPE_DIR)
			{
				printk("Not a Directory.\n");
				return -1;
			}
			cur_process->cwd = in;
			return 0;
		}
	}
	printk("Not found directory\n");
	return -1;
}

int get_curde(struct inode *cwd, struct direntry * de)
{
	struct inode *pwd;
	int i, ndir;

	//get parent dir
	inode_read(cwd, 0, (char*)de, sizeof(struct direntry));
	pwd = &inode_tbl[de->de_ino];
	ndir = num_direntry(pwd);
	for(i=2; i<ndir; i++)
	{
		inode_read(pwd, i*sizeof(struct direntry), (char*)de, sizeof(struct direntry));
		if(de->de_ino == cwd->sn_ino)
			return 0;
	}
	return -1;
}
