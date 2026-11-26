# SPDX-License-Identifier: GPL-2.0-or-later
"""The SuperMedo account's three ways in, outside the Online page: in the setup guide's card and in Play's question
when online play has no account yet. Discord, the Online ID with its password (an account made on another PC or
before), and a new account (folded until asked for). Both places offered Discord alone before 2026-11-02: a
player with an account of their own had to find the Online page. One account for SuperMedo Server and Seamless
co-op."""
import threading
import webbrowser

import bbport_theme as theme

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


def password_problem(npid, password):
    """None when SuperMedo Server takes this Online ID and password, else what to tell the player (never raises:
    the line under the boxes always answers)."""
    try:
        return L.shadnet_check_login(L.SUPERMEDO_SERVER, npid, password)
    except OSError as error:
        return _('SuperMedo Server could not be reached: {}').format(L.error_text(error))
    except Exception as error:  # an answer that is not shadNet's
        return _('Sign-in failed: {}').format(L.error_text(error))


def register_problem(npid, password, mail):
    """None when SuperMedo Server made the account, else what to tell the player (never raises)."""
    try:
        return L.shadnet_register(L.SUPERMEDO_SERVER, npid, password, mail)
    except OSError as error:
        return _('SuperMedo Server could not be reached: {}').format(L.error_text(error))
    except Exception as error:  # an answer that is not shadNet's (struct.error)
        return _('The account could not be created: {}').format(L.error_text(error))


def build(app, parent, bg, on_signed, accent='Accent.TButton'):
    """The three ways, packed into `parent` (background `bg`). A sign-in that worked fills the account
    (supermedo_npid, supermedo_pass), saves it and calls on_signed(). Returns the frame."""
    tk, ttk, px = app.tk, app.ttk, app.px
    npid_var, password_var = app.var('supermedo_npid', 'app'), app.var('supermedo_pass', 'app')
    box = tk.Frame(parent, bg=bg)
    box.pack(fill='x', anchor='w')
    line = tk.Label(box, text='', bg=bg, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), justify='left',
                    wraplength=px(520))

    def say(text, color=None):
        # The answer may come after the box is gone (the guide moved on, the question was closed).
        if line.winfo_exists():
            line.configure(text=text, fg=color or theme.TEXT_SOFT)

    def signed(npid, password):
        npid_var.set(npid)
        password_var.set(password)
        app.collect()
        if box.winfo_exists():
            on_signed()

    def label(master, text):
        return tk.Label(master, text=text, bg=bg, fg=theme.TEXT, font=(theme.BODY, 10))

    # Discord: the account is made the first time, named after the Discord name.
    def discord():
        say(_('Starting the sign-in…'), theme.MUTED)

        def on_page(page):
            webbrowser.open(page)
            app.ui_calls.put(lambda: say(_('Sign in with Discord in the page that opened, then come back here.')))

        def work():
            try:
                npid, password = L.discord_sign_in(L.COMMUNITY_SERVER, on_page)
            except Exception as error:  # a broken reply too (BadStatusLine): the line always answers
                app.ui_calls.put(lambda e=L.connection_error(error, L.COMMUNITY_SERVER): say(
                    _('Sign-in failed: {}').format(e), theme.BLOOD_LIGHT))
                return
            app.ui_calls.put(lambda: signed(npid, password))
        threading.Thread(target=work, daemon=True).start()
    ttk.Button(box, text=_('Sign in with Discord'), style=accent, command=discord).pack(anchor='w')

    # The Online ID and its password, checked with the server's own login before they are kept.
    login = tk.Frame(box, bg=bg)
    login.pack(anchor='w', pady=(px(12), 0))
    login_npid, login_password = tk.StringVar(value=''), tk.StringVar(value='')

    def password_sign_in():
        npid, password = login_npid.get().strip(), login_password.get()  # read here, not in the thread
        if not npid or not password:
            say(_('Enter your Online ID and your password.'), theme.AMBER)
            return
        say(_('Checking the account…'), theme.MUTED)

        def work():
            problem = password_problem(npid, password)
            app.ui_calls.put(lambda: say(problem, theme.BLOOD_LIGHT) if problem else signed(npid, password))
        threading.Thread(target=work, daemon=True).start()
    label(login, _('Online ID')).grid(row=0, column=0, sticky='w', padx=(0, px(12)))
    ttk.Entry(login, textvariable=login_npid, width=28).grid(row=0, column=1, sticky='w')
    label(login, _('Password')).grid(row=1, column=0, sticky='w', padx=(0, px(12)), pady=(px(6), 0))
    secret = ttk.Entry(login, textvariable=login_password, width=28, show='•')
    secret.grid(row=1, column=1, sticky='w', pady=(px(6), 0))
    secret.bind('<Return>', lambda _e: password_sign_in())
    ttk.Button(login, text=_('Sign in'), command=password_sign_in).grid(row=1, column=2, sticky='w', padx=(px(8), 0),
                                                                        pady=(px(6), 0))

    # A new account: a gold link opens its boxes.
    link = tk.Label(box, text='', bg=bg, fg=theme.GOLD, cursor='hand2', font=(theme.BODY_MEDIUM, 9))
    link.pack(anchor='w', pady=(px(10), 0))
    new = tk.Frame(box, bg=bg)
    new_npid, new_password, email = (tk.StringVar(value='') for _n in range(3))
    is_open = [False]

    def toggle(_e=None):
        is_open[0] = not is_open[0]
        link.configure(text=_('No account yet? Make one') + ('  ⌄' if is_open[0] else '  ›'))
        new.pack(anchor='w', pady=(px(6), 0), after=link) if is_open[0] else new.pack_forget()
    link.bind('<Button-1>', toggle)
    link.configure(text=_('No account yet? Make one') + '  ›')

    def create():
        npid, password, mail = new_npid.get().strip(), new_password.get(), email.get().strip()
        if not npid or not password or '@' not in mail:
            say(_('Enter a new Online ID, a password and your email address.'), theme.AMBER)
            return
        say(_('Creating the account…'), theme.MUTED)

        def work():
            problem = register_problem(npid, password, mail)
            app.ui_calls.put(lambda: say(problem, theme.BLOOD_LIGHT) if problem else signed(npid, password))
        threading.Thread(target=work, daemon=True).start()
    for row, (title, var, show) in enumerate(((_('New Online ID'), new_npid, ''), (_('New password'), new_password, '•'),
                                              (_('Email address'), email, ''))):
        label(new, title).grid(row=row, column=0, sticky='w', padx=(0, px(12)), pady=(px(6) if row else 0, 0))
        ttk.Entry(new, textvariable=var, width=28, show=show).grid(row=row, column=1, sticky='w',
                                                                   pady=(px(6) if row else 0, 0))
    ttk.Button(new, text=_('Create account'), command=create).grid(row=2, column=2, sticky='w', padx=(px(8), 0),
                                                                    pady=(px(6), 0))
    tk.Label(new, text=_('1 to 16 letters, numbers, _ or -. Other players see this name.'), bg=bg, fg=theme.MUTED,
             font=(theme.BODY, 9)).grid(row=3, column=1, columnspan=2, sticky='w', pady=(px(4), 0))
    line.pack(anchor='w', pady=(px(10), 0))
    box.toggle_new = toggle  # the tests open the new account's boxes as the link does
    return box


def ask(app):
    """Play's question when online play has no account yet: sign in here (any of the three ways), play offline
    this time, or not at all. Returns 'signed', 'offline' or None."""
    tk, ttk, px = app.tk, app.ttk, app.px
    bg = theme.BG
    win = tk.Toplevel(app.root)
    win.title('Bloodborne')
    win.configure(bg=bg)
    win.transient(app.root)
    win.resizable(False, False)
    body = tk.Frame(win, bg=bg, padx=px(22), pady=px(18))
    body.pack(fill='both', expand=True)
    tk.Label(body, text=_('Online play needs a free SuperMedo account: one for SuperMedo Server and Seamless '
                          'co-op. Sign in or make one here.'), bg=bg, fg=theme.TEXT_STRONG,
             font=(theme.BODY_SEMI, 10), justify='left', wraplength=px(520)).pack(anchor='w', pady=(0, px(14)))
    answer = [None]

    def done(value):
        answer[0] = value
        if win.winfo_exists():
            win.destroy()
    build(app, body, bg, lambda: done('signed'))
    buttons = tk.Frame(body, bg=bg)
    buttons.pack(fill='x', pady=(px(18), 0))
    ttk.Button(buttons, text=_('Cancel'), command=lambda: done(None)).pack(side='right')
    ttk.Button(buttons, text=_('Play offline this time'), command=lambda: done('offline')).pack(side='right',
                                                                                              padx=(0, px(8)))
    win.protocol('WM_DELETE_WINDOW', lambda: done(None))
    win.bind('<Escape>', lambda _e: done(None))
    win.grab_set()
    app.root.wait_window(win)
    return answer[0]
