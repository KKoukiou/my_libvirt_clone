dnl The criu libcriu.so library
dnl
dnl Copyright (C) 2016 Katerina Koukiou
dnl
dnl This library is free software; you can redistribute it and/or
dnl modify it under the terms of the GNU Lesser General Public
dnl License as published by the Free Software Foundation; either
dnl version 2.1 of the License, or (at your option) any later version.
dnl
dnl This library is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
dnl Lesser General Public License for more details.
dnl
dnl You should have received a copy of the GNU Lesser General Public
dnl License along with this library.  If not, see
dnl <http://www.gnu.org/licenses/>.
dnl

dnl Currently tested against criu 2.0; earlier
dnl versions may be possible but only with further testing
AC_DEFUN([LIBVIRT_CHECK_CRIU],[
  LIBVIRT_CHECK_PKG([CRIU], [criu], [2.0])
])

AC_DEFUN([LIBVIRT_RESULT_CRIU],[
  LIBVIRT_RESULT_LIB([CRIU])
])
