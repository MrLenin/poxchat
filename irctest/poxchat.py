"""irctest controller for PoxChat (text frontend).

Generates servlist.conf and poxchat.conf in a temporary --cfgdir,
then launches poxchat-text --no-plugins to connect to the test server.

Place this file at irctest/controllers/poxchat.py in the irctest repo,
or run with:  pytest --controller irctest.controllers.poxchat
"""

import os
import subprocess
from pathlib import Path
from typing import Optional, Set, Type

from irctest import authentication, tls
from irctest.basecontrollers import (
    BaseClientController,
    DirectoryBasedController,
    NotImplementedByController,
    TestCaseControllerConfig,
)

# PoxChat servlist.conf flag bitmask values
FLAG_CYCLE = 1
FLAG_USE_GLOBAL = 2
FLAG_USE_SSL = 4
FLAG_AUTO_CONNECT = 8
FLAG_ALLOW_INVALID = 32

# PoxChat login type constants
LOGIN_SASL = 6
LOGIN_SASLEXTERNAL = 10
LOGIN_SASL_SCRAM_SHA_256 = 12

# Map irctest mechanism enums to PoxChat login type integers
_MECHANISM_TO_LOGIN = {
    authentication.Mechanisms.plain: LOGIN_SASL,
    authentication.Mechanisms.scram_sha_256: LOGIN_SASL_SCRAM_SHA_256,
}


class PoxchatController(BaseClientController, DirectoryBasedController):
    software_name = "PoxChat"
    supported_sasl_mechanisms: Set[str] = {
        "PLAIN",
        "SCRAM-SHA-256",
    }
    supports_sts = False

    def __init__(self, test_config: TestCaseControllerConfig):
        super().__init__(test_config)
        self.poxchat_bin = os.environ.get("POXCHAT_TEXT_BIN", "poxchat-text")

    def create_config(self) -> None:
        super().create_config()

    def run(
        self,
        hostname: str,
        port: int,
        auth: Optional[authentication.Authentication],
        tls_config: Optional[tls.TlsConfig] = None,
    ) -> None:
        assert self.proc is None
        self.create_config()
        assert self.directory is not None

        # Build servlist.conf flags
        flags = FLAG_CYCLE | FLAG_AUTO_CONNECT
        use_tls = False

        if tls_config is not None:
            if not tls_config.enable:
                pass  # TLS explicitly disabled
            else:
                use_tls = True
                flags |= FLAG_USE_SSL
                # irctest uses self-signed certs
                flags |= FLAG_ALLOW_INVALID

        # Server entry: host/port or host/+port for TLS
        if use_tls:
            server_entry = f"{hostname}/+{port}"
        else:
            server_entry = f"{hostname}/{port}"

        # Login type and credentials
        login_type = 0
        sasl_user = ""
        sasl_pass = ""

        if auth is not None:
            if auth.ecdsa_key is not None:
                raise NotImplementedByController("ECDSA authentication")

            # Pick the first supported mechanism
            for mech in auth.mechanisms:
                if mech in _MECHANISM_TO_LOGIN:
                    login_type = _MECHANISM_TO_LOGIN[mech]
                    break
            else:
                raise NotImplementedByController(
                    f"SASL mechanisms: {auth.mechanisms}"
                )

            sasl_user = auth.username or ""
            sasl_pass = auth.password or ""

        # Write servlist.conf
        with self.open_file("servlist.conf") as fd:
            fd.write("v=2.16.2\n\n")
            fd.write("N=testnet\n")
            fd.write(f"I={sasl_user or 'tester'}\n")
            fd.write(f"i={sasl_user or 'tester'}2\n")
            fd.write(f"U={sasl_user or 'tester'}\n")
            fd.write("R=irctest\n")
            if sasl_pass:
                fd.write(f"P={sasl_pass}\n")
            if sasl_user:
                fd.write(f"L={login_type}\n")
            fd.write(f"F={flags}\n")
            fd.write(f"S={server_entry}\n")
            fd.write("D=0\n")
            fd.write("\n")

        # Write poxchat.conf
        nick = sasl_user or "tester"
        with self.open_file("poxchat.conf") as fd:
            fd.write(f"irc_nick1 = {nick}\n")
            fd.write(f"irc_nick2 = {nick}2\n")
            fd.write(f"irc_nick3 = {nick}3\n")
            fd.write(f"irc_user_name = {nick}\n")
            fd.write("irc_real_name = irctest\n")
            fd.write("gui_autoopen_dialog = 0\n")
            fd.write("gui_autoopen_chat = 0\n")
            fd.write("irc_reconnect_rejoin = 0\n")
            fd.write("net_auto_reconnect = 0\n")
            fd.write("net_auto_reconnectonfail = 0\n")
            fd.write("gui_network_icons = 0\n")

        # Launch poxchat-text
        self.proc = self.execute(
            [
                self.poxchat_bin,
                "--no-plugins",
                "--cfgdir",
                str(self.directory),
            ]
        )


def get_irctest_controller_class() -> Type[PoxchatController]:
    return PoxchatController
