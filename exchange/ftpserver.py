"""Tiny anonymous FTP server for the eBOX <-> PC exchange.

  out/  files for the eBOX (driver builds, batch files)   - read
  in/   files coming back from the eBOX (logs)            - write

Run:  python ftpserver.py   (allow python.exe in the Windows Firewall prompt)
Win98 ftp.exe uses active mode, which pyftpdlib supports.
"""
import os
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

root = os.path.dirname(os.path.abspath(__file__))
auth = DummyAuthorizer()
# anonymous: list/read everywhere, create/overwrite files anywhere under root
auth.add_anonymous(root, perm="elradfmw")

handler = FTPHandler
handler.authorizer = auth
handler.banner = "eBOX exchange"
handler.passive_ports = range(60000, 60100)

server = FTPServer(("0.0.0.0", 21), handler)
server.max_cons = 16
print("serving", root, "on port 21")
server.serve_forever()
