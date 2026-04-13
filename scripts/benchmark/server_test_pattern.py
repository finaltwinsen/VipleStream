#!/usr/bin/env python3
"""
VipleStream FRUC Test Pattern Generator (Server-side)
=====================================================
Displays timed visual test scenes for FRUC quality evaluation.
Run on the Sunshine server — the display will be captured and streamed.

Scenes:
  1. Static       — SMPTE color bars (MV should be ~0)
  2. Slow scroll  — horizontal scrolling grid (uniform small MV)
  3. Fast scroll  — fast horizontal scroll (larger MV)
  4. Vertical     — vertical scroll pattern
  5. Diagonal     — diagonal motion
  6. Random blocks — random moving blocks (complex MV field)

Usage:
    python server_test_pattern.py                # 10s per scene (default)
    python server_test_pattern.py --duration 15  # 15s per scene
    python server_test_pattern.py --loop         # loop scenes forever
"""

import tkinter as tk
import time
import math
import random
import argparse
import sys
import json
from datetime import datetime

# ============================================================
# Constants
# ============================================================

SCENES = [
    ("static",       "Static (Color Bars)"),
    ("slow_scroll",  "Slow Horizontal Scroll"),
    ("fast_scroll",  "Fast Horizontal Scroll"),
    ("vertical",     "Vertical Scroll"),
    ("diagonal",     "Diagonal Motion"),
    ("random_blocks","Random Blocks"),
]

COLORS_SMPTE = [
    "#C0C0C0",  # gray
    "#C0C000",  # yellow
    "#00C0C0",  # cyan
    "#00C000",  # green
    "#C000C0",  # magenta
    "#C00000",  # red
    "#0000C0",  # blue
]


class TestPatternApp:
    def __init__(self, duration=10, loop=False):
        self.duration = duration
        self.loop = loop
        self.current_scene = 0
        self.scene_start = 0
        self.frame_count = 0
        self.running = True

        # Random blocks state
        self.blocks = []

        # Setup window
        self.root = tk.Tk()
        self.root.title("VipleStream FRUC Test Pattern")
        self.root.attributes("-fullscreen", True)
        self.root.configure(bg="black")
        self.root.bind("<Escape>", lambda e: self.quit())
        self.root.bind("<q>", lambda e: self.quit())

        # Get screen size
        self.width = self.root.winfo_screenwidth()
        self.height = self.root.winfo_screenheight()

        # Canvas
        self.canvas = tk.Canvas(
            self.root, width=self.width, height=self.height,
            bg="black", highlightthickness=0
        )
        self.canvas.pack()

        # OSD font
        self.font_large = ("Consolas", 28, "bold")
        self.font_small = ("Consolas", 16)

        # Log file for scene timestamps (client can correlate)
        self.log_path = "test_pattern_log.json"
        self.log_entries = []

        self.init_random_blocks()
        self.start_time = time.time()
        self.scene_start = self.start_time

        self.log_scene_start()
        print(f"[TestPattern] Started: {len(SCENES)} scenes x {duration}s = {len(SCENES)*duration}s total")
        print(f"[TestPattern] Screen: {self.width}x{self.height}")
        print(f"[TestPattern] Press ESC or Q to quit")

        self.update()
        self.root.mainloop()

    def quit(self):
        self.running = False
        # Write log
        with open(self.log_path, "w") as f:
            json.dump({
                "scenes": self.log_entries,
                "screen": {"width": self.width, "height": self.height},
                "duration_per_scene": self.duration,
            }, f, indent=2)
        print(f"[TestPattern] Log saved to {self.log_path}")
        self.root.destroy()

    def log_scene_start(self):
        scene_id, scene_name = SCENES[self.current_scene]
        entry = {
            "scene": scene_id,
            "name": scene_name,
            "index": self.current_scene,
            "start_time": datetime.now().isoformat(),
            "start_elapsed": time.time() - self.start_time,
        }
        self.log_entries.append(entry)
        print(f"[TestPattern] Scene {self.current_scene+1}/{len(SCENES)}: {scene_name}")

    def init_random_blocks(self):
        self.blocks = []
        for _ in range(20):
            size = random.randint(40, 120)
            self.blocks.append({
                "x": random.randint(0, self.width - size),
                "y": random.randint(0, self.height - size),
                "size": size,
                "dx": random.choice([-1, 1]) * random.randint(2, 8),
                "dy": random.choice([-1, 1]) * random.randint(2, 8),
                "color": f"#{random.randint(64,255):02x}{random.randint(64,255):02x}{random.randint(64,255):02x}",
            })

    def update(self):
        if not self.running:
            return

        now = time.time()
        scene_elapsed = now - self.scene_start

        # Check scene transition
        if scene_elapsed >= self.duration:
            self.current_scene += 1
            if self.current_scene >= len(SCENES):
                if self.loop:
                    self.current_scene = 0
                else:
                    self.quit()
                    return
            self.scene_start = now
            scene_elapsed = 0
            self.log_scene_start()
            if SCENES[self.current_scene][0] == "random_blocks":
                self.init_random_blocks()

        # Draw current scene
        self.canvas.delete("all")
        scene_id = SCENES[self.current_scene][0]
        t = now - self.start_time  # global time

        if scene_id == "static":
            self.draw_static()
        elif scene_id == "slow_scroll":
            self.draw_scroll(t, speed=2)
        elif scene_id == "fast_scroll":
            self.draw_scroll(t, speed=8)
        elif scene_id == "vertical":
            self.draw_vertical_scroll(t, speed=4)
        elif scene_id == "diagonal":
            self.draw_diagonal(t, speed=4)
        elif scene_id == "random_blocks":
            self.draw_random_blocks()

        # OSD
        scene_name = SCENES[self.current_scene][1]
        remaining = max(0, self.duration - scene_elapsed)
        osd = f"{scene_name}  [{remaining:.0f}s]  Scene {self.current_scene+1}/{len(SCENES)}"
        # Shadow
        self.canvas.create_text(
            self.width // 2 + 2, 32, text=osd,
            font=self.font_large, fill="#000000", anchor="n"
        )
        self.canvas.create_text(
            self.width // 2, 30, text=osd,
            font=self.font_large, fill="#FFFFFF", anchor="n"
        )

        # Bottom info
        fps_text = f"Frame: {self.frame_count}  |  {self.width}x{self.height}"
        self.canvas.create_text(
            self.width // 2, self.height - 20, text=fps_text,
            font=self.font_small, fill="#888888", anchor="s"
        )

        self.frame_count += 1
        self.root.after(16, self.update)  # ~60fps

    # ============================================================
    # Scene Renderers
    # ============================================================

    def draw_static(self):
        """SMPTE-like color bars — expect zero motion vectors."""
        bar_w = self.width // len(COLORS_SMPTE)
        for i, color in enumerate(COLORS_SMPTE):
            x0 = i * bar_w
            x1 = x0 + bar_w if i < len(COLORS_SMPTE) - 1 else self.width
            self.canvas.create_rectangle(x0, 80, x1, self.height - 40, fill=color, outline="")

        # Center crosshair for visual reference
        cx, cy = self.width // 2, self.height // 2
        self.canvas.create_line(cx - 50, cy, cx + 50, cy, fill="white", width=2)
        self.canvas.create_line(cx, cy - 50, cx, cy + 50, fill="white", width=2)

    def draw_scroll(self, t, speed):
        """Horizontal scrolling grid — expect uniform horizontal MV."""
        offset = int(t * speed * 60) % 200

        # Grid lines
        for x in range(-200, self.width + 200, 100):
            rx = x - offset
            self.canvas.create_line(rx, 80, rx, self.height - 40, fill="#00AA00", width=2)
        for y in range(80, self.height - 40, 100):
            self.canvas.create_line(0, y, self.width, y, fill="#00AA00", width=1)

        # Moving text markers
        for x in range(-400, self.width + 400, 200):
            rx = x - offset
            self.canvas.create_text(
                rx, self.height // 2, text=f">>>  SCROLL  >>>",
                font=("Consolas", 20, "bold"), fill="#00FF00"
            )

        # Speed indicator
        self.canvas.create_text(
            100, self.height - 60, text=f"Speed: {speed}x",
            font=self.font_small, fill="#FFFF00", anchor="sw"
        )

    def draw_vertical_scroll(self, t, speed):
        """Vertical scrolling — expect uniform vertical MV."""
        offset = int(t * speed * 60) % 200

        for y in range(-200, self.height + 200, 100):
            ry = y - offset
            self.canvas.create_line(0, ry, self.width, ry, fill="#AA6600", width=2)
        for x in range(0, self.width, 100):
            self.canvas.create_line(x, 80, x, self.height - 40, fill="#AA6600", width=1)

        for y in range(-400, self.height + 400, 200):
            ry = y - offset
            self.canvas.create_text(
                self.width // 2, ry, text="VERTICAL SCROLL",
                font=("Consolas", 20, "bold"), fill="#FF8800"
            )

    def draw_diagonal(self, t, speed):
        """Diagonal motion — expect diagonal MV."""
        ox = int(t * speed * 40) % 200
        oy = int(t * speed * 40) % 200

        for i in range(-10, max(self.width, self.height) // 80 + 10):
            x = i * 80 - ox
            y = i * 80 - oy
            self.canvas.create_oval(
                x - 20, y - 20, x + 20, y + 20,
                fill="#0066FF", outline="#0088FF", width=2
            )
            self.canvas.create_oval(
                x + 40, y - 10, x + 60, y + 10,
                fill="#FF6600", outline="#FF8800", width=2
            )

        # Direction arrow
        cx, cy = self.width // 2, self.height // 2
        self.canvas.create_line(
            cx - 60, cy - 60, cx + 60, cy + 60,
            fill="white", width=3, arrow=tk.LAST
        )

    def draw_random_blocks(self):
        """Random colored blocks with independent motion — complex MV field."""
        for b in self.blocks:
            b["x"] += b["dx"]
            b["y"] += b["dy"]
            # Bounce
            if b["x"] <= 0 or b["x"] + b["size"] >= self.width:
                b["dx"] = -b["dx"]
                b["x"] = max(0, min(b["x"], self.width - b["size"]))
            if b["y"] <= 80 or b["y"] + b["size"] >= self.height - 40:
                b["dy"] = -b["dy"]
                b["y"] = max(80, min(b["y"], self.height - b["size"] - 40))

            self.canvas.create_rectangle(
                b["x"], b["y"], b["x"] + b["size"], b["y"] + b["size"],
                fill=b["color"], outline="white", width=1
            )


# ============================================================
# Main
# ============================================================

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="VipleStream FRUC Test Pattern")
    parser.add_argument("--duration", type=int, default=10, help="Seconds per scene (default: 10)")
    parser.add_argument("--loop", action="store_true", help="Loop scenes continuously")
    args = parser.parse_args()

    TestPatternApp(duration=args.duration, loop=args.loop)
