userland_chgrp = custom_target('userland-chgrp',
  input: files('main.c') + userland_libspore,
  output: 'chgrp',
  command: userland_cc + ['@INPUT@', '-o', '@OUTPUT@'])
userland_image_targets += userland_chgrp
