# SPDX-License-Identifier: GPL-3.0-or-later
"""Themed family picker. Internal INI values remain fsr411/fsr412 plus their backend."""
import ctypes
import sys
from tkinter import TclError

import bbport_fsr41 as model
import bbport_fsr411
import bbport_fsr412
import bbport_fsr41_details as details
import bbport_theme as theme


def popup_titlebar(window):
    """Keep native popup captions black, including when a dialog is reopened."""
    if sys.platform != 'win32': return

    def apply(event=None):
        if event is not None and event.widget is not window: return
        try:
            hwnd = int(window.wm_frame(), 16)
            if hwnd == window.winfo_id(): return  # Tk has not mapped its native wrapper yet.
            attribute = ctypes.windll.dwmapi.DwmSetWindowAttribute
            attribute.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint]
            attribute.restype = ctypes.c_long

            def set_value(number, value):
                data = ctypes.c_uint32(value)
                return attribute(hwnd, number, ctypes.byref(data), ctypes.sizeof(data))

            if set_value(20, 1): set_value(19, 1)  # Dark caption on Windows 10 as well.
            set_value(35, 0x00000000)  # DWMWA_CAPTION_COLOR (Windows 11): black.
            set_value(36, 0x00eeeeee)  # DWMWA_TEXT_COLOR: readable light caption text.
        except (AttributeError, OSError, ValueError, TclError):
            pass  # Older Windows versions may not expose caption-colour attributes.

    window.bind('<Map>', apply, add='+')
    window.after_idle(apply)


def statuses(app):
    caps = getattr(app, 'fsr411_support', None)
    return [bbport_fsr412.method_status(method, caps) if version == 'fsr412'
            else app.fsr411_status(method) for version, method, _title, _role in model.ROUTES]


def select(app, index):
    if not statuses(app)[index]['supported']: return False
    version, method, _title, _role = model.ROUTES[index]
    app.var('fsr41_version', 'ini').set(version)
    app.var('fsr411_backend', 'ini').set(method)
    app.var('upscaler', 'ini').set(version)
    return True


def watch(app, widget, refresh, variables):
    traces = [(var, var.trace_add('write', refresh)) for var in variables]
    app.fsr411_refreshers.append(refresh)
    def cleanup(event):
        if event.widget != widget: return
        for var, trace in traces: var.trace_remove('write', trace)
        if refresh in app.fsr411_refreshers: app.fsr411_refreshers.remove(refresh)
    widget.bind('<Destroy>', cleanup, add='+')
    refresh()


def upscaler_choice(app, parent, options, width, translate):
    tk, ttk = app.tk, app.ttk
    value = app.var('upscaler', 'ini')
    frame = ttk.Frame(parent)
    frame.setting_key = 'upscaler'
    frame.button = ttk.Menubutton(frame, width=-width)
    frame.button.pack(anchor='w')
    frame.menu = tk.Menu(frame.button, tearoff=False, background=theme.CARD, foreground=theme.TEXT,
        activebackground=theme.BLOOD, activeforeground=theme.TEXT, disabledforeground=theme.MUTED)
    frame.button.configure(menu=frame.menu)
    frame.selection = tk.StringVar()

    def choose_family():
        if value.get() in ('fsr411', 'fsr412'): return
        ready = statuses(app)
        preferred = model.route_index(app.var('fsr41_version', 'ini').get(), app.var('fsr411_backend', 'ini').get())
        index = model.preferred_route(ready, preferred)
        if index is not None: select(app, index)

    def refresh(*_args):
        frame.menu.delete(0, 'end')
        family = value.get() in ('fsr411', 'fsr412')
        added = False
        for key, text in options:
            if key in ('fsr411', 'fsr412'):
                if added: continue
                added = True
                frame.menu.add_radiobutton(label=translate('FSR 4.1 (experimental)'), value='fsr41',
                    variable=frame.selection, command=choose_family,
                    state='normal' if any(s['supported'] for s in statuses(app)) else 'disabled')
            else:
                frame.menu.add_radiobutton(label=translate(*text), value=key, variable=frame.selection,
                    command=lambda key=key: value.set(key))
        frame.selection.set('fsr41' if family else value.get())
        frame.button.configure(text=translate('FSR 4.1 (experimental)') if family else
                               translate(*dict(options).get(value.get(), ('Off',))))

    watch(app, frame, refresh, [value, app.var('fsr411_backend', 'ini')])
    return frame


def settings_button(app, parent, translate):
    ttk = app.ttk
    frame = ttk.Frame(parent)
    frame.setting_key = 'fsr411_backend'
    frame.button = ttk.Button(frame, text=translate('FSR 4.1 Settings')+' …',
                              command=lambda: open_settings(app, translate))
    frame.button.pack(anchor='w')
    if hasattr(app, 'index_entry'):
        app.index_entry(frame.button, translate('FSR 4.1 Settings'),
                        'FSR 4.1.1 FSR 4.1.2 Vulkan DX12 fsr4vk', 'fsr411_backend')
    frame.summary = ttk.Label(frame, style='Muted.TLabel', wraplength=app.px(450))
    frame.summary.pack(anchor='w', pady=(app.px(5), 0))
    def refresh(*_args):
        selected = app.var('upscaler', 'ini').get()
        frame.button.state(['!disabled' if selected in ('fsr411', 'fsr412') else 'disabled'])
        if selected in ('fsr411', 'fsr412'):
            index = model.route_index(selected, app.var('fsr411_backend', 'ini').get())
            text = translate('Selected')+': '+model.ROUTES[index][2]
        elif any(s['supported'] for s in statuses(app)):
            text = ''
        else:
            checking = any(s.get('reason') == 'checking' for s in statuses(app))
            text = translate('Checking support…' if checking else 'FSR 4.1 unavailable')
        frame.summary.configure(text=text)
        frame.summary.pack(anchor='w', pady=(app.px(5), 0)) if text else frame.summary.pack_forget()
    watch(app, frame, refresh, [app.var('upscaler', 'ini'), app.var('fsr411_backend', 'ini')])
    return frame


class MethodHint:
    """Local, read-only help on hover or keyboard focus; never selects a method."""
    def __init__(self, app, widget, text):
        self.app, self.widget, self.text = app, widget, text
        self.pending = self.window = None
        widget.bind('<Enter>', self.schedule, add='+')
        widget.bind('<FocusIn>', self.show, add='+')
        widget.bind('<Leave>', self.hide, add='+')
        widget.bind('<FocusOut>', self.hide, add='+')
        widget.configure(command=self.show)
        widget.bind('<Escape>', self.escape, add='+')
        widget.bind('<Destroy>', self.hide, add='+')

    def schedule(self, _event=None):
        self.hide()
        self.pending = self.widget.after(350, self.show)

    def show(self, _event=None):
        self.hide()
        tk, px = self.app.tk, self.app.px
        window = self.window = tk.Toplevel(self.widget)
        window.withdraw()
        window.overrideredirect(True)
        tk.Label(window, text=self.text, bg=theme.CARD, fg=theme.TEXT,
                 font=(theme.BODY, 9), justify='left', relief='solid', borderwidth=1,
                 padx=px(9), pady=px(6), wraplength=min(px(320), window.winfo_screenwidth()-px(40))).pack()
        window.update_idletasks()
        x = min(self.widget.winfo_rootx(), window.winfo_screenwidth()-window.winfo_reqwidth()-px(8))
        y = self.widget.winfo_rooty()+self.widget.winfo_height()+px(5)
        if y+window.winfo_reqheight() > window.winfo_screenheight()-px(8):
            y = self.widget.winfo_rooty()-window.winfo_reqheight()-px(5)
        window.geometry(f'+{max(0,x)}+{max(0,y)}')
        window.deiconify()

    def hide(self, _event=None):
        if self.pending is not None:
            self.widget.after_cancel(self.pending)
            self.pending = None
        if self.window is not None:
            self.window.destroy()
            self.window = None

    def escape(self, _event=None):
        self.hide()
        return 'break'


def open_settings(app, translate):
    current = getattr(app, 'fsr41_window', None)
    if current is not None and current.winfo_exists():
        current.deiconify(); current.lift(); current.focus_set()
        return current
    tk, ttk, px = app.tk, app.ttk, app.px
    window = tk.Toplevel(app.root)
    app.fsr41_window = window
    window.title('')
    window.configure(bg=theme.BG)
    window.transient(app.root)
    popup_titlebar(window)
    window.resizable(True, True)
    body = tk.Frame(window, bg=theme.BG, padx=px(12), pady=px(10))
    body.pack(fill='both', expand=True)
    tk.Label(body, text=translate('FSR 4.1 Settings'), bg=theme.BG, fg=theme.GOLD_LIGHT,
             font=(theme.HEAD_BOLD, 14), anchor='w').pack(fill='x', pady=(0, px(8)))
    holder = tk.Frame(body, bg=theme.BG)
    holder.pack(fill='both', expand=True)
    selection = tk.StringVar()
    style = ttk.Style(window)
    for name, bg in (('Fsr41.TRadiobutton', theme.BG), ('Fsr41Selected.TRadiobutton', theme.CARD)):
        style.configure(name, background=bg, font=(theme.BODY, 9), padding=0)
        style.map(name, background=[('active', bg)])
    cards, rows, hints = [], [], []
    for index, (version, method, _title, _role) in enumerate(model.ROUTES):
        if index in (0, 2):
            heading = tk.Frame(holder, bg=theme.BG)
            heading.pack(fill='x', pady=(px(7) if index else 0, px(3)))
            tk.Label(heading, text='FSR 4.1.'+version[-1],
                bg=theme.BG, fg=theme.GOLD, font=(theme.BODY, 8), anchor='w').pack(side='left', padx=px(5))
            tk.Frame(holder, height=px(1), bg=theme.BORDER).pack(fill='x')
        row = tk.Frame(holder, bg=theme.BG)
        row.pack(fill='x')
        row.columnconfigure(2, weight=1, minsize=px(155))
        row.rowconfigure(0, minsize=px(29))
        accent = tk.Frame(row, width=px(2), bg=theme.BG)
        accent.grid(row=0, column=0, sticky='ns')
        choice = tk.Frame(row, bg=theme.BG)
        choice.grid(row=0, column=1, sticky='w', padx=(px(5), px(8)), pady=px(3))
        label = 'Vulkan (fsr4vk)' if version == 'fsr411' and method == 'vk' else 'AMD '+('Vulkan' if method == 'vk' else 'DX12')
        radio = ttk.Radiobutton(choice, text=label, variable=selection, value=str(index),
            command=lambda i=index: select(app, i), style='Fsr41.TRadiobutton')
        radio.pack(side='left')
        info = tk.Button(choice, text='ⓘ', bg=theme.BG, fg=theme.GOLD, activebackground=theme.CARD,
            activeforeground=theme.GOLD_LIGHT, font=('Segoe UI', 10), relief='flat', borderwidth=0,
            highlightthickness=1, highlightbackground=theme.BG, highlightcolor=theme.GOLD,
            padx=0, pady=0, takefocus=True)
        info.pack(side='left', padx=(px(5), 0))
        hints.append(MethodHint(app, info, translate(model.HINTS[index])))
        badge = tk.Label(row, text='', bg=theme.BG, font=(theme.BODY, 9), anchor='e',
                         justify='right', wraplength=px(175))
        badge.grid(row=0, column=2, sticky='e', padx=(0, px(6)), pady=px(3))
        tk.Frame(holder, height=px(1), bg=theme.BORDER).pack(fill='x')
        cards.append((accent, radio, badge))
        rows.append((row, choice, info))
        row.bind('<Button-1>', lambda e, i=index: select(app, i))
        badge.bind('<Button-1>', lambda e, i=index: select(app, i))
    footer = tk.Frame(body, bg=theme.BG)
    footer.pack(fill='x', pady=(px(5), 0))
    download = ttk.Button(footer, text=translate('Download fsr4vk'), command=app.download_fsr411)
    download.pack(side='left')
    window.details_button = ttk.Button(footer, text=translate('Details'),
        command=lambda: open_details(app, translate))
    window.details_button.pack(side='left', padx=px(8))
    close = ttk.Button(footer, text=translate('Close'), command=window.destroy, style='Accent.TButton')
    close.pack(side='right')

    def refresh(*_args):
        states = statuses(app)
        version, method = app.var('upscaler', 'ini').get(), app.var('fsr411_backend', 'ini').get()
        chosen = model.route_index(version, method) if version in ('fsr411', 'fsr412') else -1
        selection.set(str(chosen))
        for i, (border, radio, badge) in enumerate(cards):
            status = model.code(states[i])
            bg = theme.CARD if chosen == i else theme.BG
            border.configure(bg=theme.GOLD_LIGHT if chosen == i else bg)
            row, choice, info = rows[i]
            row.configure(bg=bg); choice.configure(bg=bg)
            info.configure(bg=bg, highlightbackground=bg)
            radio.configure(style='Fsr41Selected.TRadiobutton' if chosen == i else 'Fsr41.TRadiobutton')
            radio.state(['!disabled' if states[i]['supported'] else 'disabled'])
            badge.configure(text=translate(model.STATUS_KEYS[status]), fg=model.COLORS[status], bg=bg)
        download.state(['!disabled' if bbport_fsr411.can_download(states[3],
            getattr(app, 'fsr411_support', None)) and not getattr(app, 'fsr411_downloading', False) else 'disabled'])
    watch(app, window, refresh, [app.var('upscaler', 'ini'), app.var('fsr411_backend', 'ini')])
    window.cards = cards
    window.hints = hints
    window.refresh_status = refresh
    window.bind('<Escape>', lambda e: window.destroy())
    window.update_idletasks()
    # Respect the destination display/DPI, including the translated status column.
    width = min(max(window.winfo_reqwidth(), px(400)), window.winfo_screenwidth()-px(60))
    height = min(window.winfo_reqheight(), window.winfo_screenheight()-px(80))
    window.geometry(f'{width}x{height}')
    window.minsize(width, height)
    close.focus_set()
    return window


def open_details(app, translate):
    current = getattr(app, 'fsr41_details_window', None)
    if current is not None and current.winfo_exists():
        current.deiconify(); current.lift(); current.focus_set()
        return current
    tk, ttk, px = app.tk, app.ttk, app.px
    window = tk.Toplevel(app.root)
    app.fsr41_details_window = window
    window.title('')
    window.configure(bg=theme.BG)
    window.transient(app.root)
    popup_titlebar(window)
    body = tk.Frame(window, bg=theme.BG, padx=px(14), pady=px(12))
    body.pack(fill='both', expand=True)
    tk.Label(body, text=translate('FSR 4.1 diagnostics'), bg=theme.BG, fg=theme.GOLD_LIGHT,
             font=(theme.HEAD_BOLD, 14), anchor='w').pack(fill='x', pady=(0, px(10)))
    holder = tk.Frame(body, bg=theme.CARD, padx=px(1), pady=px(1))
    holder.pack(fill='both', expand=True)
    text = tk.Text(holder, wrap='word', font=(theme.BODY, 10), bg=theme.CARD, fg=theme.TEXT,
        insertbackground=theme.TEXT, selectbackground=theme.BLOOD, relief='flat', borderwidth=0,
        padx=px(10), pady=px(8), width=76, height=26, state='disabled')
    scroll = ttk.Scrollbar(holder, command=text.yview)
    text.configure(yscrollcommand=scroll.set)
    scroll.pack(side='right', fill='y'); text.pack(fill='both', expand=True)
    for tag, color in (('heading', theme.GOLD_LIGHT), ('pass', model.COLORS[0]),
                       ('fail', model.COLORS[1]), ('info', theme.MUTED), ('skip', theme.MUTED)):
        text.tag_configure(tag, foreground=color)
    text.tag_configure('heading', font=(theme.BODY_SEMI, 11), spacing1=px(4), spacing3=px(4))
    footer = tk.Frame(body, bg=theme.BG)
    footer.pack(fill='x', pady=(px(10), 0))
    def copy_details():
        app.root.clipboard_clear()
        app.root.clipboard_append(window.report_text)
        copy.configure(text=translate('Copied'))
    copy = ttk.Button(footer, text=translate('Copy details'), command=copy_details)
    copy.pack(side='left')
    recheck = ttk.Button(footer, text=translate('Recheck support'), command=app.recheck_fsr411)
    recheck.pack(side='left', padx=px(8))
    ttk.Button(footer, text=translate('Close'), command=window.destroy, style='Accent.TButton').pack(side='right')
    def refresh(*_args):
        caps = getattr(app, 'fsr411_support', None)
        rows = details.lines(caps, statuses(app), translate)
        report = '\n'.join(value for value, _ in rows) + '\n'
        if report != getattr(window, 'report_text', None):
            window.report_text = report
            text.configure(state='normal'); text.delete('1.0', 'end')
            # The window already displays the report title above the text.
            for value, tag in rows[1:]: text.insert('end', value + '\n', tag)
            text.configure(state='disabled')
            copy.configure(text=translate('Copy details'))
        recheck.state(['disabled' if caps is None else '!disabled'])
        copy.state(['disabled' if caps is None else '!disabled'])
    watch(app, window, refresh, [app.var('upscaler', 'ini'), app.var('fsr411_backend', 'ini')])
    window.text, window.copy_button, window.recheck_button = text, copy, recheck
    window.refresh_status = refresh
    window.bind('<Escape>', lambda e: window.destroy())
    window.geometry(f'{min(px(740), window.winfo_screenwidth()-px(60))}x{min(px(620), window.winfo_screenheight()-px(80))}')
    return window
