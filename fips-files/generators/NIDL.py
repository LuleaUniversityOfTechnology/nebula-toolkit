Version = 1

import genutil as util
import IDLC

#-------------------------------------------------------------------------------
def generate(input, out_src, out_hdr) :
# if util.isDirty(Version, [input], [out_src, out_hdr]) :
    idlc = IDLC.IDLCodeGenerator()

    idlc.SetDocument(input);
    idlc.GenerateHeader(out_hdr)
    idlc.GenerateSource(out_src)


