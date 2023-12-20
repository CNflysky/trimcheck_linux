#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define VERSION "0.0.1"

extern char *optarg;

char file_name[32] = {0x00};

const char *blockdir = "/dev";

typedef struct {
  bool verbose;
  off_t offset;
  char partition[32];
  uint64_t size;
  bool checksum;
  bool notrim;
  char report_filename[32];
  uint32_t checksum_val;
  uint32_t wait_time;
} trimcheck_options_t;

typedef struct {
  uint64_t fe_logical;  /* logical offset in bytes for the start of
                         * the extent from the beginning of the file */
  uint64_t fe_physical; /* physical offset in bytes for the start
                         * of the extent from the beginning of the disk */
  uint64_t fe_length;   /* length in bytes for this extent */
  uint64_t fe_reserved64[2];
  uint32_t fe_flags; /* FIEMAP_EXTENT_* flags for this extent */
  uint32_t fe_reserved[3];
} fiemap_extent_t;

typedef struct {
  uint64_t fm_start;          /* logical offset (inclusive) at
                               * which to start mapping (in) */
  uint64_t fm_length;         /* logical length of mapping which
                               * userspace wants (in) */
  uint32_t fm_flags;          /* FIEMAP_FLAG_* flags for request (in/out) */
  uint32_t fm_mapped_extents; /* number of extents that were mapped (out) */
  uint32_t fm_extent_count;   /* size of fm_extents array (in) */
  uint32_t fm_reserved;
  fiemap_extent_t fm_extents[16]; /* array of mapped extents (out) */
} fiemap_t;

static inline void pabort(const char *msg) {
  perror(msg);
  remove(file_name);
  exit(errno);
}

static void sig_handler(int sig) { pabort("Signal received, aborting"); }

static off_t get_file_partition_offset(const char *file_name) {
  // check file exists
  if (access(file_name, F_OK) < 0)
    pabort(file_name);
  int16_t err = 0;
  // open file
  uint16_t fd = open(file_name, O_RDONLY);
  if (!fd)
    pabort("open");

  fiemap_t fm = {0x0};
  fm.fm_start = 0;
  fm.fm_length = FIEMAP_MAX_OFFSET;
  fm.fm_extent_count = 1;

  if ((err = ioctl(fd, FS_IOC_FIEMAP, &fm)) < 0) {
    close(fd);
    pabort("FS_IOC_FIEMAP");
  }
  // we only need the 1st sector's offset
  return fm.fm_extents->fe_physical; // bytes
}

static void read_offset(const char *partition, off_t addr, void *buffer,
                        const size_t len) {
  int32_t fd = open(partition, O_RDONLY);
  if (!fd)
    pabort("open");
  pread(fd, buffer, len, addr);
  close(fd);
}

static void get_partition_block_path_from_file(const char *file_name, char *ret,
                                               size_t len) {
  static struct stat s = {0x0};
  uint32_t fd = open(file_name, O_RDONLY);
  if (!fd)
    pabort("open");
  if (fstat(fd, &s) < 0)
    pabort("fstat");
  char sysfs_path_fmt[] = "/sys/dev/block/%d:%d/uevent";
  char real_sysfs_path[64] = {0x00};
  sprintf(real_sysfs_path, sysfs_path_fmt, major(s.st_dev), minor(s.st_dev));
  FILE *fp = fopen(real_sysfs_path, "r");
  if (!fp) {
    close(fd);
    pabort("fopen");
  }
  char dev_name_no_prefix[32] = {0x00};
  if (fscanf(fp, "MAJOR=%s\nMINOR=%s\nDEVNAME=%s", dev_name_no_prefix,
             dev_name_no_prefix, dev_name_no_prefix) > 0)
    sprintf(ret, "%s/%s", blockdir, dev_name_no_prefix);
  close(fd);
  fclose(fp);
}

static void create_random_file(const char *file_name, uint64_t size) {
  FILE *fp = fopen(file_name, "wb");
  if (!fp)
    pabort("fopen");
  srand(time(NULL));
  for (uint64_t i = 0; i < size; i++) {
    uint8_t ch = rand() % 256;
    fwrite(&ch, sizeof(uint8_t), 1, fp);
  }
  fsync(fileno(fp));
  fclose(fp);
}

static inline uint32_t get_buffer_crc32(void *buffer, size_t len) {
  return crc32(0x0, (uint8_t *)buffer, len);
}

static uint64_t do_trim(const char *devName) {
  char line_buf[32] = {0x00};
  char mountpoint[32] = {0x00};
  char line_fmt[32] = {0x00};
  snprintf(line_fmt, sizeof(line_fmt), "%s %s", devName, "%s");
  FILE *fp = fopen("/proc/mounts", "r");
  if (!fp)
    pabort("fopen");
  while (fgets(line_buf, sizeof(line_buf), fp) != NULL) {
    if (sscanf(line_buf, line_fmt, &mountpoint) > 0)
      break;
  }
  if (strlen(mountpoint) == 0)
    pabort("partition is not mounted");

  uint16_t fd = open(mountpoint, O_RDONLY);
  if (!fd)
    pabort("open");
  static struct fstrim_range range = {
      .len = INT_MAX,
  };
  if (ioctl(fd, FITRIM, &range) < 0)
    pabort("ioctl");
  return range.len;
}

static void save_file_detail(trimcheck_options_t *tc_option,
                             uint32_t checksum) {
  FILE *fp = fopen(tc_option->report_filename, "w");
  fprintf(fp, "partition=%s\n", tc_option->partition);
  fprintf(fp, "offset=%ld\n", tc_option->offset);
  fprintf(fp, "size=%ld\n", tc_option->size);
  fprintf(fp, "checksum=%x\n", checksum);
  fclose(fp);
}

static void verify_trim(trimcheck_options_t *tc_options) {
  if (tc_options->size == 0 || strlen(tc_options->partition) == 0) {
    fprintf(stderr, "size and/or partition not specified!\n");
    exit(EXIT_FAILURE);
  }
  char buffer[tc_options->size];
  memset(buffer, 0x00, sizeof(buffer));
  read_offset(tc_options->partition, tc_options->offset, buffer,
              sizeof(buffer));
  uint32_t checksum = get_buffer_crc32(buffer, sizeof(buffer));
  printf("Buffer checksum: 0x%x\n", checksum);
  if (!tc_options->checksum_val) {
    printf("Printing 16 bytes\n");
    for (uint16_t i = 0; i < 16; i++)
      printf("0x%x ", buffer[i]);
    printf("\n");
  }

  if (tc_options->checksum_val) {
    printf("File checksum: 0x%x\n", tc_options->checksum_val);
    if (tc_options->checksum_val == checksum)
      printf("Checksum match, Trim appears to be not working\n");
    else {
      printf("Checksum mismatch... Printing 16 bytes\n");
      for (uint16_t i = 0; i < 16; i++)
        printf("0x%x ", buffer[i]);
      printf("\n");
      printf("If those bytes are not 0x00 or 0xFF padded, then Trim may not "
             "working.\n");
      printf("If they are 0x00 or 0xFF padded, then Trim may working "
             "correctly.\n");
      printf("If you want to run the test again, Don't forget to remove %s "
             "file.\n",
             tc_options->report_filename);
    }
  }
  exit(EXIT_SUCCESS);
}

static void trimcheck(trimcheck_options_t *tc_options) {
  // workflow:
  // step 1: create a random file in current directory
  // step 2: get it's relative offset
  // step 3: get it's checksum
  // step 4: delete it
  // step 5: (default) trigger disk trim
  // step 6: generate report

  printf("Creating file %s, size %lu\n", file_name, tc_options->size);
  create_random_file(file_name, tc_options->size);

  sync();
  printf("Waiting for %d secs to ensure file is written to disk...\n",
         tc_options->wait_time);
  sleep(tc_options->wait_time);

  if (tc_options->verbose)
    printf("Reading file offset...\n");
  tc_options->offset = get_file_partition_offset(file_name);
  if (tc_options->offset == 0) {
    fprintf(stderr, "Error: File offset is 0!\n");
    remove(file_name);
    exit(EXIT_FAILURE);
  }
  if (tc_options->verbose)
    printf("File offset of partition start is %ld\n", tc_options->offset);

  get_partition_block_path_from_file(file_name, tc_options->partition,
                                     sizeof(tc_options->partition));
  if (tc_options->verbose)
    printf("The partition containing this file is %s\n", tc_options->partition);

  FILE *fp = fopen(file_name, "rb");
  if (!fp)
    pabort("fopen");
  static struct stat fstat;
  off_t filesize = 0;
  if (stat(file_name, &fstat) == 0)
    filesize = fstat.st_size;
  char file_buffer[filesize];
  memset(file_buffer, 0x0, filesize);
  if (fread(file_buffer, sizeof(uint8_t), sizeof(file_buffer), fp) !=
      filesize) {
    fclose(fp);
    pabort("fread");
  }
  fclose(fp);
  uint32_t file_crc32 = get_buffer_crc32(file_buffer, sizeof(file_buffer));

  printf("File checksum is 0x%x\n", file_crc32);
  if (tc_options->verbose) {
    printf("Printing 16 bytes\n");
    for (uint16_t i = 0; i < 16; i++)
      printf("0x%x ", file_buffer[i]);
    printf("\n");
  }

  char buffer[tc_options->size];
  memset(buffer, 0x00, sizeof(buffer));
  read_offset(tc_options->partition, tc_options->offset, buffer,
              sizeof(buffer));
  uint32_t buffer_crc32 = get_buffer_crc32(buffer, sizeof(buffer));
  printf("Buffer checksum is 0x%x\n", buffer_crc32);
  if (tc_options->verbose) {
    printf("Printing 16 bytes\n");
    for (uint16_t i = 0; i < 16; i++)
      printf("0x%x ", buffer[i]);
    printf("\n");
  }

  if (buffer_crc32 != file_crc32) {
    fprintf(stderr, "Error: checksum mismatch!\n");
    remove(file_name);
    exit(EXIT_FAILURE);
  }

  printf("Deleting test file...\n");
  remove(file_name);

  if (tc_options->notrim)
    printf("no-trim option is set, not trimming anything...\n");
  else
    printf("%lu Byte(s) trimmed on mountpoint %s\n",
           do_trim(tc_options->partition), tc_options->partition);
  printf("Saving file details for future analysis\n");
  printf("You can re-run this program to verify trim is working or not.\n");
  save_file_detail(tc_options, file_crc32);
}

static struct option longOpts[] = {{"help", no_argument, NULL, 'h'},
                                   {"offset", required_argument, NULL, 'o'},
                                   {"partition", required_argument, NULL, 'd'},
                                   {"size", required_argument, NULL, 's'},
                                   {"checksum", required_argument, NULL, 'c'},
                                   {"name", required_argument, NULL, 'n'},
                                   {"verbose", no_argument, NULL, 'v'},
                                   {"notrim", no_argument, NULL, 'N'},
                                   {"report-file", no_argument, NULL, 'f'},
                                   {"wait-time", required_argument, NULL, 'w'},
                                   {0, 0, 0, 0}};

static void print_help(char *execname) {
  const char *help_msg =
      "Usage: %s <OPTION>\n"
      "Options:\n"
      "-h, --help\t\tShow this help\n"
      "-v, --verbose\t\tVerbose output\n"
      "-o, --offset\t\tSpecify file offset, use it with -c\n"
      "-d, --partition\t\tSpecify partition to read, use it with -c\n"
      "-s, --size\t\tSpecify file size, default 1 MB (1048576 bytes)\n"
      "-c, --checksum\t\tRead chunks on disk and calculate checksum\n"
      "-n, --name\t\tSpecify a name for the file to be genereted\n"
      "-N, --no-trim\t\tDo not trim disk after deleting the file\n"
      "-f, --report-file\tFile name for report, "
      "default \"trimcheck_report.txt\"\n"
      "-w, --wait-time\t\tSpecify time to wait for file writing to disk\n"
      "\n";
  printf(help_msg, execname);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, sig_handler);
  printf("trimcheck-linux version %s\n"
         "Developed by CNflysky <cnflysky@qq.com>\n"
         "This project is licensed under GPLv3.\n",
         VERSION);
  int8_t optret = 0;
  trimcheck_options_t tc_options = {.wait_time = 10, .size = 1024 * 1024};
  srand(time(NULL));
  char rand_name[8] = {0x00};
  for (uint8_t i = 0; i < sizeof(rand_name); i++)
    rand_name[i] = 'a' + rand() % 26;
  snprintf(file_name, sizeof(file_name), "%s.bin", rand_name);
  strcpy(tc_options.report_filename, "trimcheck_report.txt");

  while ((optret = getopt_long(argc, argv, "hvo:d:s:n:cNf:w:", longOpts,
                               NULL)) > 0) {
    switch (optret) {
    case 'v':
      tc_options.verbose = 1;
      break;
    case 'o':
      tc_options.offset = atol(optarg);
      break;
    case 'w':
      tc_options.wait_time = atol(optarg);
      break;
    case 'd':
      strncpy(tc_options.partition, optarg, sizeof(tc_options.partition));
      break;
    case 's':
      tc_options.size = atol(optarg);
      break;
    case 'c':
      tc_options.checksum = true;
      break;
    case 'n':
      strncpy(file_name, optarg, sizeof(file_name));
      break;
    case 'f':
      strncpy(tc_options.report_filename, optarg,
              sizeof(tc_options.report_filename));
      break;
    case 'N':
      tc_options.notrim = true;
      break;
    default:
    case 'h':
      print_help(argv[0]);
    };
  }

  if (tc_options.checksum)
    verify_trim(&tc_options);
  // if there is a report file exists, process it only.
  if (access(tc_options.report_filename, F_OK) >= 0) {
    printf("Found report %s, processing...\n", tc_options.report_filename);
    FILE *fp = fopen(tc_options.report_filename, "r");
    if (!fp)
      pabort("fopen");
    char report_line_buf[32] = {0x00};
    fscanf(fp, "partition=%s\noffset=%ld\nsize=%ld\nchecksum=%x\n",
           tc_options.partition, &tc_options.offset, &tc_options.size,
           &tc_options.checksum_val);
    fclose(fp);
    verify_trim(&tc_options);
  }
  trimcheck(&tc_options);
  return 0;
}
