FROM php:8.2-apache

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && docker-php-ext-install mysqli \
    && rm -rf /var/lib/apt/lists/*

# Render's default web service port is 10000.
RUN sed -i 's/Listen 80/Listen 10000/' /etc/apache2/ports.conf \
    && sed -i 's/<VirtualHost \*:80>/<VirtualHost *:10000>/' /etc/apache2/sites-available/000-default.conf

WORKDIR /var/www/html
COPY . /var/www/html/

# Never copy a local secret file into the container image.
RUN rm -f /var/www/html/.env \
    && chown -R www-data:www-data /var/www/html

EXPOSE 10000
