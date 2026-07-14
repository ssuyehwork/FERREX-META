# -*- coding: utf-8 -*-
"""
批量移除 C++ 代码注释 - 图形界面版
功能：
  1. 选择文件夹，自动扫描其中的 C++ 源文件
  2. 勾选需要处理的文件
  3. 预览模式：查看处理前后差异，不会真正修改文件
  4. 执行模式：处理前自动生成 .bak 备份，再写入去除注释后的内容
"""

import os
import re
import shutil
import difflib
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from pathlib import Path

SUPPORTED_EXTS = {'.cpp', '.cc', '.cxx', '.h', '.hpp', '.hh', '.inl'}


# ---------------------------------------------------------------------------
# 核心逻辑：去除注释（状态机方式，避免误删字符串/字符字面量里的 // 或 /* */）
# ---------------------------------------------------------------------------
def strip_comments(code: str) -> str:
    result = []
    i, n = 0, len(code)
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    in_raw_string = False
    raw_delim = ""

    while i < n:
        c = code[i]
        nxt = code[i + 1] if i + 1 < n else ''

        if in_line_comment:
            if c == '\n':
                in_line_comment = False
                result.append(c)
            i += 1
            continue

        if in_block_comment:
            if c == '*' and nxt == '/':
                in_block_comment = False
                i += 2
                continue
            if c == '\n':
                result.append(c)
            i += 1
            continue

        if in_raw_string:
            # 原始字符串字面量 R"delim(...)delim"，需要找到对应的结束符 )delim"
            end_marker = ')' + raw_delim + '"'
            if code[i:i + len(end_marker)] == end_marker:
                result.append(end_marker)
                i += len(end_marker)
                in_raw_string = False
                raw_delim = ""
                continue
            result.append(c)
            i += 1
            continue

        if in_string:
            result.append(c)
            if c == '\\' and i + 1 < n:
                result.append(nxt)
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            result.append(c)
            if c == '\\' and i + 1 < n:
                result.append(nxt)
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue

        # 检测原始字符串字面量起始：R"delim(
        if c == 'R' and nxt == '"':
            m = re.match(r'R"([^()\\\s]{0,16})\(', code[i:i + 20])
            if m:
                raw_delim = m.group(1)
                start_marker = f'R"{raw_delim}('
                result.append(start_marker)
                i += len(start_marker)
                in_raw_string = True
                continue

        if c == '/' and nxt == '/':
            in_line_comment = True
            i += 2
            continue
        if c == '/' and nxt == '*':
            in_block_comment = True
            i += 2
            continue
        if c == '"':
            in_string = True
            result.append(c)
            i += 1
            continue
        if c == "'":
            in_char = True
            result.append(c)
            i += 1
            continue

        result.append(c)
        i += 1

    return ''.join(result)


def clean_trailing_blank_lines(text: str) -> str:
    """把连续3行以上的空行压缩成2行空行，让删完注释的文件不那么空洞"""
    return re.sub(r'\n[ \t]*\n[ \t]*\n+', '\n\n', text)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
class CommentStripperApp:
    def __init__(self, root):
        self.root = root
        self.root.title("C++ 批量移除注释工具")
        self.root.geometry("820x560")

        self.folder_path = tk.StringVar()
        self.file_vars = {}  # path -> BooleanVar
        self.file_list = []  # list of Path

        self._build_ui()

    def _build_ui(self):
        top = tk.Frame(self.root)
        top.pack(fill='x', padx=10, pady=10)

        tk.Entry(top, textvariable=self.folder_path, state='readonly').pack(
            side='left', fill='x', expand=True, padx=(0, 8))
        tk.Button(top, text="选择文件夹...", command=self.choose_folder).pack(side='left')

        # 文件列表区
        mid = tk.Frame(self.root)
        mid.pack(fill='both', expand=True, padx=10, pady=(0, 10))

        btn_row = tk.Frame(mid)
        btn_row.pack(fill='x')
        tk.Button(btn_row, text="全选", command=lambda: self.set_all(True)).pack(side='left')
        tk.Button(btn_row, text="全不选", command=lambda: self.set_all(False)).pack(side='left', padx=6)
        tk.Label(btn_row, text="（勾选要处理的文件）").pack(side='left', padx=10)

        canvas_frame = tk.Frame(mid)
        canvas_frame.pack(fill='both', expand=True, pady=6)

        self.canvas = tk.Canvas(canvas_frame, borderwidth=0)
        scrollbar = ttk.Scrollbar(canvas_frame, orient='vertical', command=self.canvas.yview)
        self.checklist_frame = tk.Frame(self.canvas)

        self.checklist_frame.bind(
            "<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        self.canvas.create_window((0, 0), window=self.checklist_frame, anchor='nw')
        self.canvas.configure(yscrollcommand=scrollbar.set)

        self.canvas.pack(side='left', fill='both', expand=True)
        scrollbar.pack(side='right', fill='y')

        # 底部操作区
        bottom = tk.Frame(self.root)
        bottom.pack(fill='x', padx=10, pady=(0, 10))

        tk.Button(bottom, text="预览差异", width=14, command=self.preview_diff).pack(side='left')
        tk.Button(bottom, text="执行（自动备份 .bak）", width=22, bg="#d9534f", fg="white",
                  command=self.run_strip).pack(side='left', padx=8)

        self.status_label = tk.Label(self.root, text="请先选择文件夹", anchor='w', fg="gray")
        self.status_label.pack(fill='x', padx=10, pady=(0, 8))

    def choose_folder(self):
        folder = filedialog.askdirectory(title="选择要处理的文件夹")
        if not folder:
            return
        self.folder_path.set(folder)
        self.scan_folder(folder)

    def scan_folder(self, folder):
        for widget in self.checklist_frame.winfo_children():
            widget.destroy()
        self.file_vars.clear()
        self.file_list = []

        for p in sorted(Path(folder).rglob('*')):
            if p.is_file() and p.suffix.lower() in SUPPORTED_EXTS:
                self.file_list.append(p)

        if not self.file_list:
            self.status_label.config(text="未在该文件夹找到 C++ 源文件（.cpp/.h/.hpp 等）")
            return

        for p in self.file_list:
            var = tk.BooleanVar(value=True)
            rel = os.path.relpath(p, folder)
            cb = tk.Checkbutton(self.checklist_frame, text=rel, variable=var, anchor='w', justify='left')
            cb.pack(fill='x', anchor='w')
            self.file_vars[p] = var

        self.status_label.config(text=f"共找到 {len(self.file_list)} 个文件，已默认全选")

    def set_all(self, value):
        for var in self.file_vars.values():
            var.set(value)

    def selected_files(self):
        return [p for p, var in self.file_vars.items() if var.get()]

    def preview_diff(self):
        selected = self.selected_files()
        if not selected:
            messagebox.showwarning("提示", "请先勾选至少一个文件")
            return

        preview_win = tk.Toplevel(self.root)
        preview_win.title("差异预览")
        preview_win.geometry("900x600")

        text_widget = tk.Text(preview_win, wrap='none', font=("Consolas", 10))
        text_widget.pack(fill='both', expand=True)

        vsb = ttk.Scrollbar(preview_win, orient='vertical', command=text_widget.yview)
        text_widget.configure(yscrollcommand=vsb.set)
        vsb.pack(side='right', fill='y')

        text_widget.tag_config('added', foreground='green')
        text_widget.tag_config('removed', foreground='red')
        text_widget.tag_config('header', foreground='blue')

        max_preview = 15  # 避免文件太多时预览窗口卡死
        for idx, p in enumerate(selected):
            if idx >= max_preview:
                text_widget.insert('end', f"\n... 还有 {len(selected) - max_preview} 个文件未展示，仅预览前 {max_preview} 个 ...\n")
                break
            try:
                original = p.read_text(encoding='utf-8', errors='ignore')
            except Exception as e:
                text_widget.insert('end', f"[无法读取 {p}: {e}]\n")
                continue
            cleaned = clean_trailing_blank_lines(strip_comments(original))

            text_widget.insert('end', f"\n===== {p} =====\n", 'header')
            diff = difflib.unified_diff(
                original.splitlines(keepends=True),
                cleaned.splitlines(keepends=True),
                fromfile='原始', tofile='处理后'
            )
            for line in diff:
                if line.startswith('+') and not line.startswith('+++'):
                    text_widget.insert('end', line, 'added')
                elif line.startswith('-') and not line.startswith('---'):
                    text_widget.insert('end', line, 'removed')
                else:
                    text_widget.insert('end', line)

        text_widget.config(state='disabled')

    def run_strip(self):
        selected = self.selected_files()
        if not selected:
            messagebox.showwarning("提示", "请先勾选至少一个文件")
            return

        confirm = messagebox.askyesno(
            "确认执行",
            f"即将处理 {len(selected)} 个文件：\n"
            f"- 会先生成同名 .bak 备份文件\n"
            f"- 然后覆盖写入去除注释后的内容\n\n"
            f"确定继续吗？"
        )
        if not confirm:
            return

        success, failed = 0, []
        for p in selected:
            try:
                original = p.read_text(encoding='utf-8', errors='ignore')
                cleaned = clean_trailing_blank_lines(strip_comments(original))

                backup_path = p.with_suffix(p.suffix + '.bak')
                shutil.copy2(p, backup_path)

                p.write_text(cleaned, encoding='utf-8')
                success += 1
            except Exception as e:
                failed.append((str(p), str(e)))

        msg = f"处理完成：成功 {success} 个"
        if failed:
            msg += f"，失败 {len(failed)} 个：\n" + "\n".join(f"{f}: {err}" for f, err in failed)
        messagebox.showinfo("完成", msg)
        self.status_label.config(text=msg.replace("\n", " "))


def main():
    root = tk.Tk()
    app = CommentStripperApp(root)
    root.mainloop()


if __name__ == '__main__':
    main()