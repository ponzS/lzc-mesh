"""Disposable integration identities; never reads a user's native device key."""
import base64
import hashlib
import json
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

class Identity:
 def __init__(self):
  self.key=Ed25519PrivateKey.generate()
  public=self.key.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw)
  self.public=base64.b64encode(public).decode()
  self.fingerprint=hashlib.sha256(public).hexdigest()
 def proof(self,user,device,nonce):
  message=json.dumps(['lzc-mesh/device-proof/v1',user,device,self.public,nonce],ensure_ascii=False,separators=(',',':')).encode()
  return base64.b64encode(self.key.sign(message)).decode()
