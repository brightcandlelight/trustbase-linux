#!/usr/bin/python
from trusthub_python import *

class testPlugin(TrustHubPlugin):
    def _init_(self):
        pass
    
    def initialize(self):
        print "Python plugin initialized"
        return 0
    
    def query(self, host, cert_chain):
        print "Python plugin queried"
        return RESPONSE_VALID
    
    def finalize():
        print "Python plugin finalized"
        return

myPlugin = testPlugin()
setPlugin(myPlugin)


# vim: tabstop=8 expandtab shiftwidth=4 softtabstop=4