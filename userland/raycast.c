#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "scancode.h"

// Map definition
#define mapWidth 24
#define mapHeight 24

static int worldMap[mapWidth][mapHeight] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0, 0, 0, 0, 3, 0, 3, 0, 3, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 2, 2, 0, 2, 2, 0, 0, 0, 0, 3, 0, 3, 0, 3, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 0, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 0, 0, 0, 0, 5, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 0, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 0, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}};

// Player definitions
static double posX = 22.0, posY = 12.0;
static double dirX = -1.0, dirY = 0.0;
static double planeX = 0.0, planeY = 0.66;

// Framebuffer info
static int fb_fd = -1;
static uint8_t *fb_mem = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_bpp = 32;

// Input
static char input_buf[32];

// Key state tracking for simultaneous input
typedef struct {
  bool w, a, s, d;        // WASD movement
  bool up, down, left, right;  // Arrow keys
  bool quit;
} key_state_t;

static key_state_t keys = {false};

// Colors
#define RGB_Red 0xFFFF0000
#define RGB_Green 0xFF00FF00
#define RGB_Blue 0xFF0000FF
#define RGB_White 0xFFFFFFFF
#define RGB_Yellow 0xFFFFFF00
#define RGB_Floor 0xFF333333
#define RGB_Ceil 0xFF666666

// Simple delay using nanosleep syscall
static void delay_ms(int ms) {
  struct timespec ts = {0, ms * 1000000L};
  syscall(SYS_nanosleep, &ts, NULL);
}

static void put_pixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height)
    return;
  uint32_t *pixel = (uint32_t *)(fb_mem + y * fb_pitch + x * (fb_bpp / 8));
  *pixel = color;
}

static void clear_screen(void) {
  uint32_t ceil_col = RGB_Ceil;
  uint32_t floor_col = RGB_Floor;

  for (uint32_t row = 0; row < fb_height; row++) {
    uint32_t col_val = (row < fb_height / 2) ? ceil_col : floor_col;
    uint32_t *line = (uint32_t *)(fb_mem + row * fb_pitch);
    for (uint32_t col = 0; col < fb_width; col++) {
      line[col] = col_val;
    }
  }
}

static void draw_vert_line(int x, int drawStart, int drawEnd, uint32_t color) {
  if (drawStart < 0)
    drawStart = 0;
  if (drawEnd >= (int)fb_height)
    drawEnd = fb_height - 1;

  for (int y = drawStart; y <= drawEnd; y++) {
    put_pixel(x, y, color);
  }
}

// Gun bob + flash state
static double gun_bob_time = 0.0;

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = 0; dx < w; dx++)
      put_pixel(x + dx, y + dy, color);
}

// firing_flash: set to ~6 when player fires, counts down each frame
static int firing_flash = 0;

static void draw_gun(int bob_y) {
  int scale = (int)fb_height / 200;
  if (scale < 1) scale = 1;
  int S = scale;

  // Anchor point = right edge of receiver (where grip meets slide)
  // Barrel extends LEFT from here, hand hangs DOWN-RIGHT from here
  int ax = (int)fb_width - 80*S + bob_y / 2;   // x: right side
  int ay = (int)fb_height - 60*S + bob_y;       // y: gun centerline (vertical mid of barrel)

  uint32_t BLK = 0xFF101010;
  uint32_t GN1 = 0xFF1C1C1C;
  uint32_t GN2 = 0xFF383838;
  uint32_t GN3 = 0xFF545454;
  uint32_t GN4 = 0xFF787878;
  uint32_t GN5 = 0xFF9C9C9C;
  uint32_t SK1 = 0xFF8B4513;
  uint32_t SK2 = 0xFFB5651D;
  uint32_t SK3 = 0xFFCD853F;
  uint32_t SK4 = 0xFFDEB887;
  uint32_t TAN = 0xFF8B7355;

  // === BARREL (namlu) — extends LEFT from ax ===
  // ax-200 .. ax-80 = barrel shaft
  draw_rect(ax - 200*S, ay -  9*S, 120*S, 18*S, GN2);   // barrel body
  draw_rect(ax - 200*S, ay -  9*S, 120*S,  3*S, GN4);   // top highlight
  draw_rect(ax - 200*S, ay +  6*S, 120*S,  3*S, GN1);   // bottom shadow
  draw_rect(ax - 200*S, ay -  9*S,   3*S, 18*S, GN1);   // left (muzzle) face

  // Muzzle block (slightly taller than barrel)
  draw_rect(ax - 212*S, ay - 13*S,  14*S, 26*S, GN3);
  draw_rect(ax - 212*S, ay - 13*S,   4*S, 26*S, GN1);   // left face
  draw_rect(ax - 212*S, ay - 13*S,  14*S,  4*S, GN4);   // top

  // Bore hole (muzzle opening — leftmost face)
  draw_rect(ax - 214*S, ay -  6*S,   6*S, 12*S, BLK);
  draw_rect(ax - 213*S, ay -  4*S,   4*S,  8*S, BLK);

  // Front sight (tiny blade on top of barrel, near muzzle)
  draw_rect(ax - 196*S, ay - 13*S,   8*S,  6*S, GN4);
  draw_rect(ax - 195*S, ay - 16*S,   6*S,  4*S, GN5);

  // === RECEIVER / SLIDE — from ax-80 to ax ===
  draw_rect(ax -  80*S, ay - 13*S,  80*S, 26*S, GN2);   // main slide
  draw_rect(ax -  80*S, ay - 13*S,  80*S,  4*S, GN3);   // top strip
  draw_rect(ax -  80*S, ay +  9*S,  80*S,  4*S, GN1);   // bottom strip
  draw_rect(ax -  80*S, ay - 13*S,   4*S, 26*S, GN1);   // left face

  // Ejection port cutout (top of slide)
  draw_rect(ax -  55*S, ay - 13*S,  28*S, 14*S, GN1);
  draw_rect(ax -  53*S, ay - 11*S,  24*S, 10*S, BLK);

  // Slide serrations (near grip end, top of slide)
  for (int i = 0; i < 4; i++) {
    draw_rect(ax - 18*S - i*6*S, ay - 13*S, 4*S, 7*S, GN1);
    draw_rect(ax - 18*S - i*6*S, ay - 13*S, 4*S, 3*S, GN4);
  }

  // Rear sight (top of slide, near grip)
  draw_rect(ax - 26*S, ay - 18*S,  20*S,  7*S, GN3);
  draw_rect(ax - 26*S, ay - 18*S,   5*S,  7*S, GN1);   // left wall of notch
  draw_rect(ax - 11*S, ay - 18*S,   5*S,  7*S, GN1);   // right wall of notch
  draw_rect(ax - 23*S, ay - 20*S,  12*S,  4*S, BLK);   // notch gap

  // === GRIP (sap) — hangs DOWN from receiver ===
  draw_rect(ax -  50*S, ay + 13*S,  36*S, 40*S, GN2);   // grip body
  draw_rect(ax -  50*S, ay + 13*S,   4*S, 40*S, GN1);   // left face
  draw_rect(ax -  18*S, ay + 13*S,   4*S, 40*S, GN3);   // right face
  // Grip texture
  draw_rect(ax -  46*S, ay + 16*S,  24*S, 30*S, GN1);
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 2; col++)
      draw_rect(ax - 44*S + col*10*S, ay + 18*S + row*8*S, 8*S, 6*S, GN2);

  // Trigger guard
  draw_rect(ax -  60*S, ay + 13*S,  46*S,  4*S, GN2);   // top of guard (below receiver)
  draw_rect(ax -  60*S, ay + 13*S,   4*S, 20*S, GN2);   // left post
  draw_rect(ax -  18*S, ay + 13*S,   4*S, 16*S, GN2);   // right post
  draw_rect(ax -  60*S, ay + 29*S,  46*S,  4*S, GN2);   // bottom of guard

  // Trigger
  draw_rect(ax -  42*S, ay + 15*S,   8*S, 16*S, GN3);
  draw_rect(ax -  40*S, ay + 17*S,   4*S, 10*S, GN4);

  // === HAND (el) — comes from right, wraps grip ===
  // Palm over grip
  draw_rect(ax -  8*S, ay +  8*S,  50*S, 45*S, SK3);    // palm body
  draw_rect(ax -  8*S, ay +  8*S,   5*S, 45*S, SK2);    // left edge (touches grip)
  draw_rect(ax + 38*S, ay +  8*S,   5*S, 45*S, SK1);    // right shadow
  draw_rect(ax -  8*S, ay +  5*S,  50*S,  5*S, SK4);    // top knuckle row

  // Forearm (extends right off screen)
  draw_rect(ax + 10*S, ay + 30*S,  80*S, 40*S, SK2);
  draw_rect(ax + 10*S, ay + 30*S,   5*S, 40*S, SK1);
  draw_rect(ax + 10*S, ay + 30*S,  80*S,  4*S, SK3);    // top edge

  // Thumb (sticks UP-LEFT from palm, rests on slide)
  draw_rect(ax - 14*S, ay -  8*S,  18*S, 18*S, SK3);
  draw_rect(ax - 16*S, ay - 14*S,  12*S, 10*S, SK4);    // thumb tip
  draw_rect(ax - 16*S, ay -  8*S,   5*S, 18*S, SK2);    // shadow

  // Index finger (along trigger guard)
  draw_rect(ax -  50*S, ay +  8*S,  14*S, 10*S, SK3);
  draw_rect(ax -  56*S, ay +  7*S,   8*S,  8*S, SK4);   // fingertip
  draw_rect(ax -  50*S, ay + 14*S,  14*S,  4*S, TAN);   // crease

  // Knuckle bumps across top of palm
  draw_rect(ax -  4*S, ay +  2*S,   8*S,  6*S, SK4);
  draw_rect(ax +  6*S, ay +  1*S,   8*S,  7*S, SK4);
  draw_rect(ax + 16*S, ay +  2*S,   8*S,  6*S, SK4);
  draw_rect(ax + 26*S, ay +  3*S,   8*S,  5*S, SK3);

  // === MUZZLE FLASH ===
  if (firing_flash > 0) {
    int fx = ax - 216*S;   // just left of muzzle
    int fy = ay;           // barrel centerline
    draw_rect(fx - 16*S, fy -  6*S, 16*S, 12*S, 0xFFFFFFCC);
    draw_rect(fx - 28*S, fy - 10*S, 16*S, 20*S, 0xFFFFDD00);
    draw_rect(fx - 36*S, fy -  6*S, 10*S, 12*S, 0xFFFFDD00);
    draw_rect(fx - 24*S, fy - 20*S, 12*S, 10*S, 0xFFFF6600);
    draw_rect(fx - 24*S, fy + 10*S, 12*S, 10*S, 0xFFFF6600);
    draw_rect(fx - 46*S, fy -  4*S,  8*S, 10*S, 0xFFFF8800);
    draw_rect(fx - 54*S, fy -  2*S,  8*S,  6*S, 0xFFFFFFAA);
    firing_flash--;
  }
}

static void render_frame(void) {
  clear_screen();

  for (int x = 0; x < (int)fb_width; x++) {
    double cameraX = 2 * x / (double)fb_width - 1;
    double rayDirX = dirX + planeX * cameraX;
    double rayDirY = dirY + planeY * cameraX;

    int mapX = (int)posX;
    int mapY = (int)posY;

    double sideDistX;
    double sideDistY;

    double deltaDistX = (rayDirX == 0) ? 1e30 : fabs(1 / rayDirX);
    double deltaDistY = (rayDirY == 0) ? 1e30 : fabs(1 / rayDirY);
    double perpWallDist;

    int stepX;
    int stepY;

    int hit = 0;
    int side;

    if (rayDirX < 0) {
      stepX = -1;
      sideDistX = (posX - mapX) * deltaDistX;
    } else {
      stepX = 1;
      sideDistX = (mapX + 1.0 - posX) * deltaDistX;
    }
    if (rayDirY < 0) {
      stepY = -1;
      sideDistY = (posY - mapY) * deltaDistY;
    } else {
      stepY = 1;
      sideDistY = (mapY + 1.0 - posY) * deltaDistY;
    }

    while (hit == 0) {
      if (sideDistX < sideDistY) {
        sideDistX += deltaDistX;
        mapX += stepX;
        side = 0;
      } else {
        sideDistY += deltaDistY;
        mapY += stepY;
        side = 1;
      }
      if (worldMap[mapX][mapY] > 0)
        hit = 1;
    }

    if (side == 0)
      perpWallDist = (sideDistX - deltaDistX);
    else
      perpWallDist = (sideDistY - deltaDistY);

    int lineHeight = (int)(fb_height / perpWallDist);

    int drawStart = -lineHeight / 2 + fb_height / 2;
    if (drawStart < 0)
      drawStart = 0;
    int drawEnd = lineHeight / 2 + fb_height / 2;
    if (drawEnd >= (int)fb_height)
      drawEnd = fb_height - 1;

    uint32_t color;
    switch (worldMap[mapX][mapY]) {
    case 1:
      color = RGB_Red;
      break;
    case 2:
      color = RGB_Green;
      break;
    case 3:
      color = RGB_Blue;
      break;
    case 4:
      color = RGB_White;
      break;
    default:
      color = RGB_Yellow;
      break;
    }

    if (side == 1) {
      uint8_t a = (color >> 24) & 0xFF;
      uint8_t r = ((color >> 16) & 0xFF) / 2;
      uint8_t g = ((color >> 8) & 0xFF) / 2;
      uint8_t b = (color & 0xFF) / 2;
      color = (a << 24) | (r << 16) | (g << 8) | b;
    }

    draw_vert_line(x, drawStart, drawEnd, color);
  }

  // Draw gun overlay (bob based on movement)
  bool moving = keys.w || keys.s || keys.up || keys.down;
  if (moving) {
    gun_bob_time += 0.18;
  } else {
    gun_bob_time *= 0.85;  // dampen when stopped
  }
  int bob_y = (int)(sin(gun_bob_time) * 6.0);
  draw_gun(bob_y);

  lseek(fb_fd, 0, SEEK_SET);
  write(fb_fd, fb_mem, fb_height * fb_pitch);
}

static void handle_input(void) {
  double moveSpeed = 0.5;
  double rotSpeed = 0.2;
  
  scancode_event_t event;
  
  // Update key state from all available scancodes
  while (ioctl(0, KBDSCANCODE_READ, &event) > 0) {
    bool is_pressed = !event.is_release;
    
    // Handle extended scancodes (arrow keys)
    if (event.is_extended) {
      switch (event.scancode) {
      case SCANCODE_UP:
        keys.up = is_pressed;
        break;
      case SCANCODE_DOWN:
        keys.down = is_pressed;
        break;
      case SCANCODE_LEFT:
        keys.left = is_pressed;
        break;
      case SCANCODE_RIGHT:
        keys.right = is_pressed;
        break;
      }
      continue;
    }
    
    // Handle normal scancodes
    switch (event.scancode) {
    case SCANCODE_W:
      keys.w = is_pressed;
      break;
    case SCANCODE_A:
      keys.a = is_pressed;
      break;
    case SCANCODE_S:
      keys.s = is_pressed;
      break;
    case SCANCODE_D:
      keys.d = is_pressed;
      break;
    case SCANCODE_Q:
    case SCANCODE_ESC:
      if (is_pressed) keys.quit = true;
      break;
    }
  }
  
  // Apply movement based on all currently held keys
  // Forward/backward movement
  if (keys.w || keys.up) {
    if (worldMap[(int)(posX + dirX * moveSpeed)][(int)(posY)] == 0)
      posX += dirX * moveSpeed;
    if (worldMap[(int)(posX)][(int)(posY + dirY * moveSpeed)] == 0)
      posY += dirY * moveSpeed;
  }
  if (keys.s || keys.down) {
    if (worldMap[(int)(posX - dirX * moveSpeed)][(int)(posY)] == 0)
      posX -= dirX * moveSpeed;
    if (worldMap[(int)(posX)][(int)(posY - dirY * moveSpeed)] == 0)
      posY -= dirY * moveSpeed;
  }
  
  // Rotation
  if (keys.a || keys.left) {
    double oldDirX = dirX;
    dirX = dirX * cos(rotSpeed) - dirY * sin(rotSpeed);
    dirY = oldDirX * sin(rotSpeed) + dirY * cos(rotSpeed);
    double oldPlaneX = planeX;
    planeX = planeX * cos(rotSpeed) - planeY * sin(rotSpeed);
    planeY = oldPlaneX * sin(rotSpeed) + planeY * cos(rotSpeed);
  }
  if (keys.d || keys.right) {
    double oldDirX = dirX;
    dirX = dirX * cos(-rotSpeed) - dirY * sin(-rotSpeed);
    dirY = oldDirX * sin(-rotSpeed) + dirY * cos(-rotSpeed);
    double oldPlaneX = planeX;
    planeX = planeX * cos(-rotSpeed) - planeY * sin(-rotSpeed);
    planeY = oldPlaneX * sin(-rotSpeed) + planeY * cos(-rotSpeed);
  }
  
  if (keys.quit) exit(0);
}

static int detect_fb_dimensions(int fd) {
  off_t fb_size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  if (fb_size <= 0) {
    printf("Warning: lseek(SEEK_END) failed, falling back to defaults\n");
    fb_width = 1024;
    fb_height = 768;
    fb_pitch = fb_width * 4;
    return 0;
  }

  static const struct {
    uint32_t w, h;
  } resolutions[] = {{640, 480},   {800, 600},   {1024, 600},  {1024, 768},
                     {1152, 864},  {1280, 720},  {1280, 768},  {1280, 800},
                     {1280, 960},  {1280, 1024}, {1360, 768},  {1366, 768},
                     {1400, 1050}, {1440, 900},  {1600, 900},  {1600, 1200},
                     {1680, 1050}, {1920, 800},  {1920, 1080}, {1920, 1200},
                     {2560, 1080}, {2560, 1440}, {3840, 2160}, {0, 0}};

  for (int i = 0; resolutions[i].w != 0; i++) {
    uint32_t w = resolutions[i].w;
    uint32_t h = resolutions[i].h;
    uint32_t pitch = w * 4;
    if ((off_t)(pitch * h) == fb_size) {
      fb_width = w;
      fb_height = h;
      fb_pitch = pitch;
      return 1;
    }
  }

  printf("Warning: unknown resolution (fb_size=%ld), guessing from size\n",
         (long)fb_size);
  for (uint32_t w = 640; w <= 4096; w += 64) {
    if (fb_size % (w * 4) == 0) {
      uint32_t h = (uint32_t)(fb_size / (w * 4));
      if (h >= 480 && h <= 2160) {
        fb_width = w;
        fb_height = h;
        fb_pitch = w * 4;
        printf("Guessed: %ux%u\n", w, h);
        return 1;
      }
    }
  }

  fb_width = 1024;
  fb_height = 768;
  fb_pitch = fb_width * 4;
  return 0;
}

int main(void) {
  int flags = fcntl(0, F_GETFL, 0);
  fcntl(0, F_SETFL, flags | O_NONBLOCK);

  fb_fd = open("/dev/fb0", O_RDWR);
  if (fb_fd < 0) {
    printf("Error: Cannot open /dev/fb0\n");
    return 1;
  }

  detect_fb_dimensions(fb_fd);

  size_t fb_size = (size_t)fb_pitch * fb_height;
  fb_mem = (uint8_t *)malloc(fb_size);
  if (!fb_mem) {
    printf("Error: Cannot allocate framebuffer memory (%zu bytes)\n", fb_size);
    close(fb_fd);
    return 1;
  }
  memset(fb_mem, 0, fb_size);

  printf("Raycast initialized: %ux%u (pitch=%u, total=%zu bytes)\n", fb_width,
         fb_height, fb_pitch, fb_size);
  printf("Controls: W/A/S/D or arrow keys to move, Q to quit\n");

  // Enable scancode mode for raw keyboard input
  if (keyboard_set_scancode_mode(0, true) < 0) {
    printf("Warning: Failed to enable scancode mode, falling back to ASCII\n");
  }

  while (1) {
    handle_input();
    render_frame();
    delay_ms(16);
  }

  free(fb_mem);
  close(fb_fd);
  return 0;
}