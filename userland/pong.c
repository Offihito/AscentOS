#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/syscall.h>

// Game constants
#define PADDLE_WIDTH  15
#define PADDLE_HEIGHT 80
#define BALL_SIZE     12
#define PADDLE_SPEED  8
#define BALL_SPEED    5
#define AI_SPEED      2

// Colors (ARGB)
#define COLOR_BG      0xFF1a1a2e
#define COLOR_PADDLE  0xFF00d4ff
#define COLOR_BALL    0xFFff6b6b
#define COLOR_NET     0xFF4a4a6a
#define COLOR_TEXT    0xFFffffff

// Framebuffer info (will be queried)
static int fb_fd = -1;
static uint8_t *fb_mem = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_bpp = 32;

// Game state
static int player_y = 0;
static int ai_y = 0;
static int ball_x = 0;
static int ball_y = 0;
static int ball_vx = 0;
static int ball_vy = 0;
static int player_score = 0;
static int ai_score = 0;

// Input
static char input_buf[32];

// Simple delay using nanosleep syscall
static void delay_ms(int ms) {
  struct timespec ts = {0, ms * 1000000L};
  syscall(SYS_nanosleep, &ts, NULL);
}

static void put_pixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height) return;
  uint32_t *pixel = (uint32_t *)(fb_mem + y * fb_pitch + x * (fb_bpp / 8));
  *pixel = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      put_pixel(x + dx, y + dy, color);
    }
  }
}

static void clear_screen(void) {
  // Fast fill using memset-style row copy
  uint32_t bg = COLOR_BG;
  for (uint32_t row = 0; row < fb_height; row++) {
    uint32_t *line = (uint32_t *)(fb_mem + row * fb_pitch);
    for (uint32_t col = 0; col < fb_width; col++) {
      line[col] = bg;
    }
  }
}

static void draw_net(void) {
  int net_x = fb_width / 2;
  for (int y = 0; y < (int)fb_height; y += 20) {
    fill_rect(net_x - 2, y, 4, 10, COLOR_NET);
  }
}

static void draw_paddle(int x, int y) {
  fill_rect(x, y, PADDLE_WIDTH, PADDLE_HEIGHT, COLOR_PADDLE);
}

static void draw_ball(int x, int y) {
  fill_rect(x, y, BALL_SIZE, BALL_SIZE, COLOR_BALL);
}

static void draw_score(void) {
  // Score is displayed in the terminal output at game start
}

static void reset_ball(void) {
  ball_x = fb_width / 2 - BALL_SIZE / 2;
  ball_y = fb_height / 2 - BALL_SIZE / 2;
  ball_vx = BALL_SPEED;
  ball_vy = 0;
}

static void init_game(void) {
  player_y = fb_height / 2 - PADDLE_HEIGHT / 2;
  ai_y = fb_height / 2 - PADDLE_HEIGHT / 2;
  reset_ball();
  srand((unsigned int)time(NULL));
}

static void update_ai(void) {
  // Only react when ball is moving towards AI
  if (ball_vx < 0) return;

  int ai_center = ai_y + PADDLE_HEIGHT / 2;
  int ball_center = ball_y + BALL_SIZE / 2;

  // Add some reaction delay - only move when ball is close
  if (ball_x < (int)fb_width / 2) return;

  // Imperfect tracking with larger dead zone
  if (ai_center < ball_center - 30) {
    ai_y += AI_SPEED;
  } else if (ai_center > ball_center + 30) {
    ai_y -= AI_SPEED;
  }

  // Keep AI paddle in bounds
  if (ai_y < 0) ai_y = 0;
  if (ai_y + PADDLE_HEIGHT > (int)fb_height) ai_y = fb_height - PADDLE_HEIGHT;
}

static void update_ball(void) {
  ball_x += ball_vx;
  ball_y += ball_vy;

  // Top/bottom wall collision
  if (ball_y <= 0 || ball_y + BALL_SIZE >= (int)fb_height) {
    ball_vy = -ball_vy;
    ball_y = (ball_y <= 0) ? 0 : (int)fb_height - BALL_SIZE;
  }

  // Player paddle collision
  int player_x = 30;
  if (ball_x <= player_x + PADDLE_WIDTH &&
      ball_x + BALL_SIZE >= player_x &&
      ball_y + BALL_SIZE >= player_y &&
      ball_y <= player_y + PADDLE_HEIGHT) {
    ball_vx = abs(ball_vx) + 1;
    int hit_pos = (ball_y + BALL_SIZE / 2) - player_y;
    ball_vy = (hit_pos - PADDLE_HEIGHT / 2) / 5;
    ball_x = player_x + PADDLE_WIDTH;
  }

  // AI paddle collision
  int ai_x = fb_width - 30 - PADDLE_WIDTH;
  if (ball_x + BALL_SIZE >= ai_x &&
      ball_x <= ai_x + PADDLE_WIDTH &&
      ball_y + BALL_SIZE >= ai_y &&
      ball_y <= ai_y + PADDLE_HEIGHT) {
    ball_vx = -(abs(ball_vx) + 1);
    int hit_pos = (ball_y + BALL_SIZE / 2) - ai_y;
    ball_vy = (hit_pos - PADDLE_HEIGHT / 2) / 5;
    ball_x = ai_x - BALL_SIZE;
  }

  // Scoring
  if (ball_x + BALL_SIZE < 0) {
    ai_score++;
    draw_score();
    reset_ball();
  } else if (ball_x > (int)fb_width) {
    player_score++;
    draw_score();
    reset_ball();
  }

  // Speed limit
  if (abs(ball_vx) > 12) ball_vx = (ball_vx > 0) ? 12 : -12;
  if (abs(ball_vy) > 8)  ball_vy = (ball_vy > 0) ? 8  : -8;
}

static void handle_input(void) {
  int bytes = read(0, input_buf, sizeof(input_buf) - 1);
  if (bytes > 0) {
    input_buf[bytes] = '\0';

    // Parse escape sequences for arrow keys
    if (bytes >= 3 && input_buf[0] == '\033' && input_buf[1] == '[') {
      switch (input_buf[2]) {
        case 'A': player_y -= PADDLE_SPEED; break; // Up
        case 'B': player_y += PADDLE_SPEED; break; // Down
      }
    } else {
      for (int i = 0; i < bytes; i++) {
        switch (input_buf[i]) {
          case 'w': case 'W': player_y -= PADDLE_SPEED; break;
          case 's': case 'S': player_y += PADDLE_SPEED; break;
          case 'q': case 'Q': case '\033': exit(0); break;
        }
      }
    }

    // Keep player paddle in bounds
    if (player_y < 0) player_y = 0;
    if (player_y + PADDLE_HEIGHT > (int)fb_height)
      player_y = fb_height - PADDLE_HEIGHT;
  }
}

static void render(void) {
  clear_screen();
  draw_net();
  draw_paddle(30, player_y);
  draw_paddle(fb_width - 30 - PADDLE_WIDTH, ai_y);
  draw_ball(ball_x, ball_y);

  // Flush entire backbuffer to /dev/fb0
  lseek(fb_fd, 0, SEEK_SET);
  write(fb_fd, fb_mem, fb_height * fb_pitch);
}

// ---------------------------------------------------------------------------
// Detect framebuffer dimensions from the file size reported by /dev/fb0.
//
// framebuffer.c sets fb_node->length = fb->height * fb->pitch, so
// lseek(SEEK_END) gives us the exact byte count.  We then search a table of
// common (width × height) pairs — pitch is always width*4 on this platform —
// and pick the first one whose total matches.
// ---------------------------------------------------------------------------
static int detect_fb_dimensions(int fd) {
  // Get exact size from the VFS node length
  off_t fb_size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  if (fb_size <= 0) {
    printf("Warning: lseek(SEEK_END) failed, falling back to defaults\n");
    fb_width  = 1024;
    fb_height = 768;
    fb_pitch  = fb_width * 4;
    return 0;
  }

  // Table of every common resolution we might encounter
  static const struct { uint32_t w, h; } resolutions[] = {
    { 640,  480 },
    { 800,  600 },
    { 1024, 600 },
    { 1024, 768 },
    { 1152, 864 },
    { 1280, 720 },
    { 1280, 768 },
    { 1280, 800 },
    { 1280, 960 },
    { 1280,1024 },
    { 1360, 768 },
    { 1366, 768 },
    { 1400,1050 },
    { 1440, 900 },
    { 1600, 900 },
    { 1600,1200 },
    { 1680,1050 },
    { 1920, 800 },
    { 1920,1080 },
    { 1920,1200 },
    { 2560,1080 },
    { 2560,1440 },
    { 3840,2160 },
    { 0, 0 }   // sentinel
  };

  for (int i = 0; resolutions[i].w != 0; i++) {
    uint32_t w = resolutions[i].w;
    uint32_t h = resolutions[i].h;
    uint32_t pitch = w * 4;          // 32 bpp, no padding on this platform
    if ((off_t)(pitch * h) == fb_size) {
      fb_width  = w;
      fb_height = h;
      fb_pitch  = pitch;
      return 1;
    }
  }

  // Last resort: assume square-ish 32-bpp and derive from total bytes.
  // fb_size = height * width * 4  →  try pitch = sqrt(fb_size/4) rounded.
  printf("Warning: unknown resolution (fb_size=%ld), guessing from size\n",
         (long)fb_size);
  // Walk possible widths: common widths are multiples of 64
  for (uint32_t w = 640; w <= 4096; w += 64) {
    if (fb_size % (w * 4) == 0) {
      uint32_t h = (uint32_t)(fb_size / (w * 4));
      if (h >= 480 && h <= 2160) {
        fb_width  = w;
        fb_height = h;
        fb_pitch  = w * 4;
        printf("Guessed: %ux%u\n", w, h);
        return 1;
      }
    }
  }

  // Absolute fallback
  fb_width  = 1024;
  fb_height = 768;
  fb_pitch  = fb_width * 4;
  return 0;
}

int main(void) {
  // Open framebuffer
  fb_fd = open("/dev/fb0", O_RDWR);
  if (fb_fd < 0) {
    printf("Error: Cannot open /dev/fb0\n");
    return 1;
  }

  // Detect actual resolution from the node's reported size
  detect_fb_dimensions(fb_fd);

  // Allocate backbuffer
  size_t fb_size = (size_t)fb_pitch * fb_height;
  fb_mem = (uint8_t *)malloc(fb_size);
  if (!fb_mem) {
    printf("Error: Cannot allocate framebuffer memory (%zu bytes)\n", fb_size);
    close(fb_fd);
    return 1;
  }
  memset(fb_mem, 0, fb_size);

  printf("Pong initialized: %ux%u (pitch=%u, total=%zu bytes)\n",
         fb_width, fb_height, fb_pitch, fb_size);
  printf("Controls: W/S or arrow keys to move, Q to quit\n");

  init_game();

  // Game loop
  while (1) {
    handle_input();
    update_ai();
    update_ball();
    render();
    delay_ms(16); // ~60 FPS
  }

  free(fb_mem);
  close(fb_fd);
  return 0;
}