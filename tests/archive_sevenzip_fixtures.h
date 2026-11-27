#pragma once

#include <array>
#include <string_view>

// Own payloads encoded by 7-Zip 26.04, with timestamps omitted. These reference
// fixtures cover providers the libarchive writer cannot generate (notably AES).
// The two encrypted fixtures use the public test-only password fixture-pass.
namespace sevenzip_fixtures {
  struct Fixture {
    std::string_view name, base64;
    bool encrypted;
  };
  inline constexpr std::array archives{
      Fixture{
          "lzma2",
          "N3q8ryccAASaBuANmAAAAAAAAAAhAAAAAAAAAJnRV33gA58AIV0AMxpKrAxyv5zanmNHqsOZmeDDqYxL6ZAnJ2yd2V+/0oAAAAAA"
          "gTMHrg/QluR8nz9HQQQPcQ0SU4hghjAKn1eiUtsAXOYo2A2V3IcqCWLKvmO62WJnXisETLhTcFG2GtrXtA092eWE0lMBOBKEYbkU"
          "1S0yimodr/0T6INZ8b8jKaqYKph/xyRtNN4ds4N/kGPQABcGKQEJbwAHCwEAASMDAQEFXQAQAAAMgJoKAThXT8EAAA==",
          false
      },
      Fixture{
          "lzma1",
          "N3q8ryccAASYjR/tIgAAAAAAAACaAAAAAAAAAK2Yh14AMxpKrAxyv5zanmNHqsOZmeDDqYxL6ZAnJ2yd2V+/0oAAAQQGAAEJIgAH"
          "CwEAASMDAQEFXQAQAAAMg6AACA0CCYHACgEdZMSvBFRxFAAABQMOAYAZCQAAAAAAAAAAABFHAGYAbwBsAGQAZQByAAAAZgBvAGwA"
          "ZABlAHIALwBmAGkAcgBzAHQALgB0AHgAdAAAAHMAZQBjAG8AbgBkAC4AdAB4AHQAAAAZABUOAQAQgO1BIICkgSCApIEAAA==",
          false
      },
      Fixture{
          "copy",
          "N3q8ryccAAR9hrF6oAMAAAAAAACKAAAAAAAAABQU3W9maXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApm"
          "aXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3Qg"
          "cGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxv"
          "YWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZp"
          "cnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBw"
          "YXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9hZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKZmlyc3QgcGF5bG9h"
          "ZApmaXJzdCBwYXlsb2FkCmZpcnN0IHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQK"
          "c2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKc2Vjb25kIHBheWxvYWQKAQQGAAIJgcCB4AAHCwIA"
          "AQEAAQEADIHAgeAACAoBHWTErwRUcRQAAAUDDgGAEUcAZgBvAGwAZABlAHIAAABmAG8AbABkAGUAcgAvAGYAaQByAHMAdAAuAHQA"
          "eAB0AAAAcwBlAGMAbwBuAGQALgB0AHgAdAAAABkAFQ4BABCA7UEggKSBIICkgQAA",
          false
      },
      Fixture{
          "deflate",
          "N3q8ryccAARZqY+uKQAAAAAAAACaAAAAAAAAAKiOYWvtx8EJACAIAMB/U7SaZEEQKerH7Z1D8X53rqhNBn8EOHrZpnvRx27VBgEE"
          "BgABCSkABwsBAAEDBAEIDIOgAAgNAgmBwAoBHWTErwRUcRQAAAUDDgGAGQ8AAAAAAAAAAAAAAAAAAAARRwBmAG8AbABkAGUAcgAA"
          "AGYAbwBsAGQAZQByAC8AZgBpAHIAcwB0AC4AdAB4AHQAAABzAGUAYwBvAG4AZAAuAHQAeAB0AAAAGQAVDgEAEIDtQSCApIEggKSB"
          "AAA=",
          false
      },
      Fixture{
          "bzip2",
          "N3q8ryccAAR6s/4RTwAAAAAAAACaAAAAAAAAAOBEtrxCWmgxMUFZJlNZY3Jr/wAA0FGAABBAAC8l3CAgAHBQwAAKlIGRgnUBqSSk"
          "klLgYAsCoGAaBYFwbAsDQGQZA0CoNg9BcD8XckU4UJBjcmv/AQQGAAEJTwAHCwEAAQMEAgIMg6AACA0CCYHACgEdZMSvBFRxFAAA"
          "BQMOAYAZDwAAAAAAAAAAAAAAAAAAABFHAGYAbwBsAGQAZQByAAAAZgBvAGwAZABlAHIALwBmAGkAcgBzAHQALgB0AHgAdAAAAHMA"
          "ZQBjAG8AbgBkAC4AdAB4AHQAAAAZABUOAQAQgO1BIICkgSCApIEAAA==",
          false
      },
      Fixture{
          "ppmd",
          "N3q8ryccAATss4QnKQAAAAAAAACaAAAAAAAAACcbhssAZgVYJBVmsk1GACvWo1KOZeYAAiD7LYjJqMUC2EJPhycNgKZFdsX4AAEE"
          "BgABCSkABwsBAAEjAwQBBQQAAAEADIOgAAgNAgmBwAoBHWTErwRUcRQAAAUDDgGAGQkAAAAAAAAAAAARRwBmAG8AbABkAGUAcgAA"
          "AGYAbwBsAGQAZQByAC8AZgBpAHIAcwB0AC4AdAB4AHQAAABzAGUAYwBvAG4AZAAuAHQAeAB0AAAAGQAVDgEAEIDtQSCApIEggKSB"
          "AAA=",
          false
      },
      Fixture{
          "nonsolid",
          "N3q8ryccAAQWC7z9PwAAAAAAAACaAAAAAAAAAOidsrzgAb8AF10AMxpKrAxyv5zanmNHqsOZmeDDqYOnAAAA4AHfABhdADmZSJGx"
          "aZYH4/a/A/etw23LPAFoihAAAAABBAYAAgkfIAAHCwIAASEhAQABISEBAAyBwIHgAAgKAR1kxK8EVHEUAAAFAw4BgBkMAAAAAAAA"
          "AAAAAAAAEUcAZgBvAGwAZABlAHIAAABmAG8AbABkAGUAcgAvAGYAaQByAHMAdAAuAHQAeAB0AAAAcwBlAGMAbwBuAGQALgB0AHgA"
          "dAAAABkAFQ4BABCA7UEggKSBIICkgQAA",
          false
      },
      Fixture{
          "encrypted-data",
          "N3q8ryccAARWrfqJMAAAAAAAAACqAAAAAAAAAORu3Ki/ksRyd3wrtTJ636zhxykWaJd5d8EqCW9bjR1ZsQIqrxVKhq+WgOUEES5V"
          "sPKZIlEBBAYAAQkwAAcLAQACJAbxBwESUw+vQUsnZqbTgeB5vYT4e5dHISEBAAEADCmDoAAIDQIJgcAKAR1kxK8EVHEUAAAFAw4B"
          "gBkEAAAAABFHAGYAbwBsAGQAZQByAAAAZgBvAGwAZABlAHIALwBmAGkAcgBzAHQALgB0AHgAdAAAAHMAZQBjAG8AbgBkAC4AdAB4"
          "AHQAAAAZABUOAQAQgO1BIICkgSCApIEAAA==",
          true
      },
      Fixture{
          "encrypted-header",
          "N3q8ryccAAS/Fryr4AAAAAAAAAAwAAAAAAAAAHOkGoicyqz8xmOa2Wq7slKU0m/wsM2U0pig9Xcl10wk/Ul7hPMQwoeNP7jZFZDJ"
          "9lQUWej35Tczt1zhlxHieQ3T7D+I6mRSGHuHv9s01QgOX9Uw6dACTaA6HBoQ33YjJZN9cgDVakFHgbjqAfif5Rd55u4hTVwgAmUq"
          "7FgtIVjIFZrRGkVmvL0FmW4wd9120s5CskPUg5npqMDDJlmbLSVKoV3Rwfj6I9OSqqaWw2lmZQER6BNlVLVIBtk9+KHZOQLQmMVs"
          "Y6TEfss2TzR+36WPiElK3p35t98DiYH4NKLaaHmxPRcGMAEJgLAABwsBAAEkBvEHARJTDxySV3WLfftaiGHTMoAllz0MgKoKAR7I"
          "uH0AAA==",
          true
      },
  };

} // namespace sevenzip_fixtures
